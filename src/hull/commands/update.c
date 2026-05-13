/*
 * commands/update.c — hull self-update
 *
 * Downloads the latest GitHub release for this binary's OS/arch, verifies
 * SHA-256 against hull.sha256 from the same release, and atomically
 * replaces the running binary via rename(2).
 *
 *   hull update                — install latest if newer than current
 *   hull update --check        — print "update available" / "up to date"
 *   hull update --channel=beta — include pre-releases (no beta channel yet;
 *                                 same as stable currently)
 *   hull update --force        — reinstall current version
 *   hull update --repo=org/name — override the GitHub repo
 *
 * No external dependencies — uses keel's HTTPS client + the embedded
 * Mozilla CA bundle (D4) for trust.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/update.h"
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
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__)
# include <mach-o/dyld.h>      /* _NSGetExecutablePath */
# include <limits.h>
#elif defined(__linux__) || defined(__COSMOPOLITAN__)
# include <limits.h>
#endif

#ifndef HL_VERSION
#define HL_VERSION "dev"
#endif

#define HL_DEFAULT_REPO "artalis-io/hull"

/* ── Platform detection (for asset selection) ───────────────────── */

static const char *update_platform(void)
{
#if defined(__COSMOPOLITAN__)
    return "cosmo";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "darwin-arm64";
#elif defined(__APPLE__)
    return "cosmo"; /* no darwin-x86_64 native artifact — fall back to APE */
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x86_64";
#elif defined(__linux__)
    return "cosmo"; /* no native linux-arm64 yet — APE fallback */
#else
    return "cosmo";
#endif
}

/* ── Resolve the running binary's path ──────────────────────────── */

static int resolve_self_path(char *out, size_t out_sz)
{
#if defined(__APPLE__)
    uint32_t bufsz = (uint32_t)out_sz;
    if (_NSGetExecutablePath(out, &bufsz) != 0) return -1;
    char resolved[PATH_MAX];
    if (realpath(out, resolved)) {
        size_t rl = strlen(resolved);
        if (rl + 1 > out_sz) return -1;
        memcpy(out, resolved, rl + 1);
    }
    return 0;
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", out, out_sz - 1);
    if (n <= 0) return -1;
    out[n] = '\0';
    return 0;
#else
    /* Cosmo APE and others — fall back to argv[0] resolution.
     * The dispatch layer passes argv[0] via env->hull_exe. */
    (void)out; (void)out_sz;
    return -1;
#endif
}

/* ── Simple JSON field extraction ───────────────────────────────── */

/* Find a "key":"<value>" entry in a flat JSON blob and copy the value
 * into out. Returns 0 on success, -1 if not found / truncated. This is
 * a deliberately tiny parser sufficient for tag_name / name / browser_download_url. */
static int json_extract_str(const char *json, const char *key,
                             char *out, size_t out_sz)
{
    char needle[64];
    int nlen = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (nlen <= 0 || (size_t)nlen >= sizeof(needle)) return -1;
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += nlen;
    /* skip whitespace and ':' */
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

/* ── HTTPS GET using keel + embedded CA bundle ──────────────────── */

static int https_get(const char *url, char **out_body, size_t *out_len,
                      KlAllocator *alloc, KlTlsCtx *tls_ctx)
{
    KlTlsConfig tls_cfg = {
        .ctx         = tls_ctx,
        .factory     = (KlTlsFactory)kl_tls_mbedtls_create,
        .ctx_destroy = (void (*)(KlTlsCtx *))kl_tls_mbedtls_ctx_destroy,
    };
    KlClientConfig cfg = {
        .timeout_ms        = 30000,
        .max_response_size = 200 * 1024 * 1024,  /* 200 MB (cosmo APE is ~6 MB) */
        .tls               = &tls_cfg,
    };
    KlRedirectConfig redir = { .max_redirects = 10 };
    KlClientResponse resp;
    memset(&resp, 0, sizeof(resp));

    KlClientHeader hdr = { .name = "User-Agent", .value = "hull-update/" HL_VERSION };

    int rc = kl_redirect_request(alloc, &cfg, &redir, "GET", url,
                                  &hdr, 1, NULL, 0, &resp);
    if (rc != 0) {
        kl_client_response_free(&resp);
        return -1;
    }
    if (resp.status < 200 || resp.status >= 300) {
        fprintf(stderr, "hull update: %s returned HTTP %d\n", url, resp.status);
        kl_client_response_free(&resp);
        return -1;
    }
    /* Steal the body buffer (response_free would otherwise drop it).
     * KlClientResponse.body is malloc'd via the allocator. */
    *out_len = resp.body_len;
    *out_body = (char *)resp.body;
    resp.body = NULL;
    resp.body_len = 0;
    kl_client_response_free(&resp);
    return 0;
}

/* ── SHA-256 via mbedTLS (already linked into hull) ──────────────── */

extern int mbedtls_sha256(const unsigned char *input, size_t ilen,
                           unsigned char output[32], int is224);

static int sha256_hex(const unsigned char *data, size_t len, char hex[65])
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

/* Find "<sha256-hex>  <asset>" line in a checksum manifest and copy the
 * 64-char hex into out. */
static int find_checksum(const char *manifest, size_t mlen,
                         const char *asset, char hex_out[65])
{
    char asset_pat[128];
    int alen = snprintf(asset_pat, sizeof(asset_pat), "  %s", asset);
    if (alen <= 0 || (size_t)alen >= sizeof(asset_pat)) return -1;

    const char *p = manifest;
    const char *end = manifest + mlen;
    while (p < end) {
        /* Each line is "<hex>  <asset>\n" with hex = 64 chars */
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t ll = eol ? (size_t)(eol - p) : (size_t)(end - p);
        if (ll >= 66 + strlen(asset)) {
            /* Compare the asset name part (offset 66 from line start: 64 hex + "  ") */
            const char *anchor = p + 64;
            if (anchor[0] == ' ' && anchor[1] == ' ' &&
                strncmp(anchor + 2, asset, strlen(asset)) == 0 &&
                /* Make sure asset name matches exactly (no prefix collision) */
                (anchor[2 + strlen(asset)] == '\n' ||
                 anchor[2 + strlen(asset)] == '\r' ||
                 anchor[2 + strlen(asset)] == '\0' ||
                 anchor + 2 + strlen(asset) >= end)) {
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

/* ── Atomic replace via rename ──────────────────────────────────── */

static int atomic_replace(const char *target, const char *new_path)
{
    /* rename(2) atomically replaces target with new_path on Linux/macOS.
     * On Windows (which Cosmo APE may run on), rename(2) fails if target
     * exists — Cosmo's libc emulates POSIX semantics where possible. */
    if (rename(new_path, target) != 0)
        return -1;
    return 0;
}

/* ── Main handler ────────────────────────────────────────────────── */

int hl_cmd_update(int argc, char **argv, const HlCommandEnv *env)
{
    int check_only = 0;
    int force = 0;
    const char *repo = HL_DEFAULT_REPO;
    /* --channel=stable|beta — reserved; not wired yet (no separate beta channel
     * exists today). Accept and ignore so install/CI scripts can pass it. */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) {
            check_only = 1;
        } else if (strcmp(argv[i], "--force") == 0) {
            force = 1;
        } else if (strncmp(argv[i], "--channel=", 10) == 0) {
            /* reserved — accepted, not used */
        } else if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
            i++;  /* consume value; reserved */
        } else if (strncmp(argv[i], "--repo=", 7) == 0) {
            repo = argv[i] + 7;
        } else if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) {
            repo = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stdout,
                "Usage: hull update [options]\n"
                "\n"
                "Options:\n"
                "  --check               Check for an update without installing\n"
                "  --force               Reinstall even if version matches\n"
                "  --channel=stable|beta Release channel (default: stable)\n"
                "  --repo=ORG/NAME       Override the GitHub repo (default: " HL_DEFAULT_REPO ")\n"
                "\n");
            return 0;
        }
    }

    /* ── 1. Set up TLS using the embedded CA bundle ─────────────── */
    KlAllocator alloc = kl_allocator_default();
    KlTlsCtx *tls = NULL;
    {
        const unsigned char *cab = NULL;
        size_t cab_len = 0;
        if (hl_embedded_ca_bundle(&cab, &cab_len) == 0) {
            tls = kl_tls_mbedtls_client_ctx_create_from_buf(cab, cab_len, &alloc);
        }
        if (!tls) {
            /* Fall back to a system path */
            const char *paths[] = {
                "/etc/ssl/cert.pem",
                "/etc/ssl/certs/ca-certificates.crt",
                "/etc/pki/tls/certs/ca-bundle.crt",
                NULL,
            };
            for (const char **p = paths; *p && !tls; p++) {
                if (access(*p, R_OK) == 0)
                    tls = kl_tls_mbedtls_client_ctx_create(*p, &alloc);
            }
        }
        if (!tls) {
            fprintf(stderr, "hull update: no CA bundle available — cannot verify HTTPS\n");
            return 1;
        }
    }

    /* ── 2. Fetch the latest release metadata ───────────────────── */
    char api_url[256];
    snprintf(api_url, sizeof(api_url),
             "https://api.github.com/repos/%s/releases/latest", repo);
    fprintf(stdout, "hull update: checking %s …\n", repo);

    char *meta_body = NULL;
    size_t meta_len = 0;
    if (https_get(api_url, &meta_body, &meta_len, &alloc, tls) != 0) {
        fprintf(stderr, "hull update: failed to fetch release metadata\n");
        kl_tls_mbedtls_ctx_destroy(tls);
        return 1;
    }

    char latest_tag[64];
    if (json_extract_str(meta_body, "tag_name", latest_tag, sizeof(latest_tag)) != 0) {
        fprintf(stderr, "hull update: could not parse release metadata\n");
        kl_free(&alloc, meta_body, meta_len);
        kl_tls_mbedtls_ctx_destroy(tls);
        return 1;
    }
    kl_free(&alloc, meta_body, meta_len);

    /* Strip leading 'v' from tag for comparison */
    const char *latest_ver = latest_tag;
    if (latest_ver[0] == 'v') latest_ver++;

    fprintf(stdout, "hull update: current  %s\n", HL_VERSION);
    fprintf(stdout, "hull update: latest   %s\n", latest_tag);

    if (!force && strcmp(HL_VERSION, latest_ver) == 0) {
        fprintf(stdout, "hull update: already up to date\n");
        kl_tls_mbedtls_ctx_destroy(tls);
        return 0;
    }

    if (check_only) {
        fprintf(stdout, "hull update: update available (run `hull update` to install)\n");
        kl_tls_mbedtls_ctx_destroy(tls);
        return 0;
    }

    /* ── 3. Download the asset ──────────────────────────────────── */
    const char *plat = update_platform();
    char asset_name[64];
    snprintf(asset_name, sizeof(asset_name), "hull-%s", plat);

    char asset_url[256];
    snprintf(asset_url, sizeof(asset_url),
             "https://github.com/%s/releases/download/%s/%s",
             repo, latest_tag, asset_name);
    fprintf(stdout, "hull update: downloading %s …\n", asset_name);

    char *binary = NULL;
    size_t binary_len = 0;
    if (https_get(asset_url, &binary, &binary_len, &alloc, tls) != 0) {
        fprintf(stderr, "hull update: failed to download %s\n", asset_url);
        kl_tls_mbedtls_ctx_destroy(tls);
        return 1;
    }
    fprintf(stdout, "hull update: downloaded %zu bytes\n", binary_len);

    /* ── 4. Verify SHA-256 against the release manifest ─────────── */
    char sha_url[256];
    snprintf(sha_url, sizeof(sha_url),
             "https://github.com/%s/releases/download/%s/hull.sha256",
             repo, latest_tag);

    char *manifest = NULL;
    size_t manifest_len = 0;
    if (https_get(sha_url, &manifest, &manifest_len, &alloc, tls) != 0) {
        fprintf(stderr, "hull update: failed to download checksum manifest\n");
        kl_free(&alloc, binary, binary_len);
        kl_tls_mbedtls_ctx_destroy(tls);
        return 1;
    }

    char expected[65];
    if (find_checksum(manifest, manifest_len, asset_name, expected) != 0) {
        fprintf(stderr, "hull update: no checksum entry for %s in hull.sha256\n",
                asset_name);
        kl_free(&alloc, binary, binary_len);
        kl_free(&alloc, manifest, manifest_len);
        kl_tls_mbedtls_ctx_destroy(tls);
        return 1;
    }
    kl_free(&alloc, manifest, manifest_len);

    char actual[65];
    if (sha256_hex((const unsigned char *)binary, binary_len, actual) != 0) {
        fprintf(stderr, "hull update: SHA-256 computation failed\n");
        kl_free(&alloc, binary, binary_len);
        kl_tls_mbedtls_ctx_destroy(tls);
        return 1;
    }

    if (strcmp(expected, actual) != 0) {
        fprintf(stderr, "hull update: SHA-256 mismatch\n");
        fprintf(stderr, "  expected: %s\n  actual:   %s\n", expected, actual);
        kl_free(&alloc, binary, binary_len);
        kl_tls_mbedtls_ctx_destroy(tls);
        return 1;
    }
    fprintf(stdout, "hull update: SHA-256 verified (%s)\n", actual);

    kl_tls_mbedtls_ctx_destroy(tls);

    /* ── 5. Atomic replace ──────────────────────────────────────── */
    char self_path[PATH_MAX];
    if (resolve_self_path(self_path, sizeof(self_path)) != 0) {
        /* Fall back to env->hull_exe (argv[0] from the dispatch layer) */
        if (env && env->hull_exe) {
            char resolved[PATH_MAX];
            if (realpath(env->hull_exe, resolved))
                snprintf(self_path, sizeof(self_path), "%s", resolved);
            else
                snprintf(self_path, sizeof(self_path), "%s", env->hull_exe);
        } else {
            fprintf(stderr, "hull update: cannot determine own binary path\n");
            kl_free(&alloc, binary, binary_len);
            return 1;
        }
    }

    /* Write to <self>.new in the same directory (same filesystem for atomic rename) */
    char new_path[PATH_MAX + 8];
    int n = snprintf(new_path, sizeof(new_path), "%s.new", self_path);
    if (n < 0 || (size_t)n >= sizeof(new_path)) {
        fprintf(stderr, "hull update: install path too long\n");
        kl_free(&alloc, binary, binary_len);
        return 1;
    }

    int fd = open(new_path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) {
        fprintf(stderr, "hull update: cannot create %s: %s\n",
                new_path, strerror(errno));
        kl_free(&alloc, binary, binary_len);
        return 1;
    }
    size_t written = 0;
    while (written < binary_len) {
        ssize_t w = write(fd, binary + written, binary_len - written);
        if (w <= 0) {
            fprintf(stderr, "hull update: write failed: %s\n", strerror(errno));
            close(fd);
            unlink(new_path);
            kl_free(&alloc, binary, binary_len);
            return 1;
        }
        written += (size_t)w;
    }
    /* fsync the new binary to disk before the rename so a power-loss
     * doesn't leave a half-written executable behind. */
    fsync(fd);
    close(fd);
    kl_free(&alloc, binary, binary_len);

    if (chmod(new_path, 0755) != 0) {
        fprintf(stderr, "hull update: chmod failed: %s\n", strerror(errno));
        unlink(new_path);
        return 1;
    }

    if (atomic_replace(self_path, new_path) != 0) {
        fprintf(stderr, "hull update: rename failed: %s\n", strerror(errno));
        unlink(new_path);
        return 1;
    }

    fprintf(stdout, "hull update: installed %s → %s\n", latest_tag, self_path);
    return 0;
}
