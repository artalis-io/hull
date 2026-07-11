/*
 * commands/platform.c - `hull platform install <flavor>` / `list`.
 *
 * Fetch signed per-flavor platform libraries from the release matching this
 * hull's version into $HOME/.hull/platform/, so `hull build --flavor=<flavor>`
 * can link them without building from source.
 *
 * Trust chain (identical to `hull tools install`):
 *   1. hl_release_io_fetch_verified_manifest downloads hull.sha256 +
 *      hull.sha256.sig and verifies the Ed25519 signature against the
 *      embedded release pubkey.
 *   2. For each asset: look up its SHA-256 in the verified manifest, download,
 *      recompute the SHA-256, constant-time compare, atomic-write.
 *
 * Network-only: compiled when HL_ENABLE_HTTP_CLIENT is on (the fetch needs
 * Keel's HTTPS client). The builder hull is normally a full release binary.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/platform.h"

#ifdef HL_ENABLE_HTTP_CLIENT

#include "hull/release_io.h"
#include "hull/module_resolver.h"   /* HlBuildFlavor / hl_build_flavor_* */

#include <keel/allocator.h>
#include <keel/tls_mbedtls.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef HL_VERSION
#define HL_VERSION "dev"
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define HL_PLATFORM_DEFAULT_REPO "artalis-io/hull"

/* GitHub tags carry a leading "v"; HL_VERSION usually does not. */
static int compose_tag(char *out, size_t out_sz)
{
    const char *v = HL_VERSION;
    int n = (v[0] == 'v') ? snprintf(out, out_sz, "%s", v)
                          : snprintf(out, out_sz, "v%s", v);
    return (n > 0 && (size_t)n < out_sz) ? 0 : -1;
}

/* $HOME/.hull/platform, created 0700. */
static int platform_cache_dir(char *out, size_t out_sz)
{
    const char *home = getenv("HOME");
    if (!home || !*home) {
        fprintf(stderr, "hull platform: HOME is not set\n");
        return -1;
    }
    char hull[PATH_MAX];
    if ((size_t)snprintf(hull, sizeof(hull), "%s/.hull", home) >= sizeof(hull))
        return -1;
    if (mkdir(hull, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "hull platform: cannot create %s: %s\n", hull, strerror(errno));
        return -1;
    }
    if ((size_t)snprintf(out, out_sz, "%s/.hull/platform", home) >= out_sz)
        return -1;
    if (mkdir(out, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "hull platform: cannot create %s: %s\n", out, strerror(errno));
        return -1;
    }
    return 0;
}

/* Constant-time compare of two 64-char hex SHA-256 strings. The values are
 * public (manifest checksum + computed digest), so a timing leak reveals
 * nothing; the constant-time form matches the rest of Hull's hash compares. */
static int ct_hex_eq(const char *a, const char *b)
{
    unsigned char diff = 0;
    for (int i = 0; i < 64; i++)
        diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

/* Build the published asset names for a flavor on this platform.
 * Returns the count (1 native, 2 cosmo); names written into assets[]. */
static int flavor_assets(const HlBuildFlavor *f, const char *plat,
                         char assets[2][160])
{
    if (strcmp(plat, "cosmo") == 0) {
        snprintf(assets[0], 160, "%s.x86_64-cosmo.a", f->asset);
        snprintf(assets[1], 160, "%s.aarch64-cosmo.a", f->asset);
        return 2;
    }
    snprintf(assets[0], 160, "%s-%s.a", f->asset, plat);
    return 1;
}

static int install_asset(const char *asset, const char *repo, const char *tag,
                         KlAllocator *alloc, KlTlsCtx *tls,
                         const char *manifest, size_t manifest_len,
                         const char *cache_dir)
{
    /* Look up the signed checksum BEFORE downloading. */
    char expected[65];
    if (hl_release_io_find_checksum(manifest, manifest_len, asset, expected) != 0) {
        fprintf(stderr, "hull platform: no checksum entry for %s in hull.sha256 "
                        "(this release may not publish that flavor)\n", asset);
        return -1;
    }

    char url[512];
    int un = snprintf(url, sizeof(url),
             "https://github.com/%s/releases/download/%s/%s", repo, tag, asset);
    if (un < 0 || (size_t)un >= sizeof(url)) {
        /* --repo is user-controllable; a pathological value truncates the URL.
         * Fail closed rather than fetch from a truncated target. (C-audit Low.) */
        fprintf(stderr, "hull platform: download URL too long (check --repo)\n");
        return -1;
    }
    fprintf(stdout, "hull platform: downloading %s ...\n", asset);

    char *body = NULL;
    size_t body_len = 0;
    if (hl_release_io_get(url, &body, &body_len, alloc, tls, "hull-platform") != 0) {
        fprintf(stderr, "hull platform: failed to download %s\n", url);
        return -1;
    }

    char actual[65];
    if (hl_release_io_sha256_hex((const unsigned char *)body, body_len, actual) != 0) {
        fprintf(stderr, "hull platform: SHA-256 computation failed\n");
        kl_free(alloc, body, body_len);
        return -1;
    }
    if (!ct_hex_eq(expected, actual)) {
        fprintf(stderr, "hull platform: SHA-256 MISMATCH for %s\n"
                        "  expected %s\n  actual   %s\n", asset, expected, actual);
        kl_free(alloc, body, body_len);
        return -1;
    }

    char dest[PATH_MAX];
    if ((size_t)snprintf(dest, sizeof(dest), "%s/%s", cache_dir, asset) >= sizeof(dest)) {
        fprintf(stderr, "hull platform: cache path overflow\n");
        kl_free(alloc, body, body_len);
        return -1;
    }
    if (hl_release_io_atomic_write(dest, body, body_len, 0644) != 0) {
        fprintf(stderr, "hull platform: failed to write %s\n", dest);
        kl_free(alloc, body, body_len);
        return -1;
    }
    kl_free(alloc, body, body_len);
    fprintf(stdout, "hull platform: installed %s (SHA-256 verified)\n", dest);
    return 0;
}

static int cmd_install(const char *flavor, const char *repo)
{
    const HlBuildFlavor *f = hl_build_flavor_find(flavor);
    if (!f) {
        const HlBuildFlavor *all = NULL;
        int count = hl_build_flavor_all(&all);
        fprintf(stderr, "hull platform: unknown flavor '%s' (valid:", flavor);
        for (int i = 0; i < count; i++)
            fprintf(stderr, " %s", all[i].name);
        fprintf(stderr, ")\n");
        return 1;
    }
    if (strcmp(f->name, "full") == 0) {
        fprintf(stdout, "hull platform: the 'full' platform is embedded in hull; "
                        "nothing to install.\n");
        return 0;
    }

    char cache_dir[PATH_MAX];
    if (platform_cache_dir(cache_dir, sizeof(cache_dir)) != 0) return 1;

    char tag[128];
    if (compose_tag(tag, sizeof(tag)) != 0) {
        fprintf(stderr, "hull platform: cannot compose release tag from version\n");
        return 1;
    }

    KlAllocator alloc = kl_allocator_default();
    KlTlsCtx *tls = hl_release_io_open_tls(&alloc);
    if (!tls) {
        fprintf(stderr, "hull platform: TLS init failed (no CA bundle available)\n");
        return 1;
    }

    char *manifest = NULL;
    size_t manifest_len = 0;
    if (hl_release_io_fetch_verified_manifest(repo, tag, &alloc, tls, "hull-platform",
                                              &manifest, &manifest_len) != 0) {
        kl_tls_mbedtls_ctx_destroy(tls);
        return 1;
    }

    char assets[2][160];
    int n = flavor_assets(f, hl_release_io_platform(), assets);

    int rc = 0;
    for (int i = 0; i < n; i++) {
        if (install_asset(assets[i], repo, tag, &alloc, tls,
                          manifest, manifest_len, cache_dir) != 0) {
            rc = 1;
            break;
        }
    }

    kl_free(&alloc, manifest, manifest_len);
    kl_tls_mbedtls_ctx_destroy(tls);

    if (rc == 0)
        fprintf(stdout, "hull platform: %s ready. "
                        "`hull build --flavor=%s` will use it.\n", f->name, f->name);
    return rc;
}

static int cmd_list(void)
{
    char cache_dir[PATH_MAX];
    if (platform_cache_dir(cache_dir, sizeof(cache_dir)) != 0) return 1;
    const char *plat = hl_release_io_platform();

    const HlBuildFlavor *all = NULL;
    int count = hl_build_flavor_all(&all);
    fprintf(stdout, "Build flavors (platform: %s, cache: %s):\n", plat, cache_dir);
    for (int i = 0; i < count; i++) {
        if (strcmp(all[i].name, "full") == 0) {
            fprintf(stdout, "  %-14s embedded\n", all[i].name);
            continue;
        }
        char assets[2][160];
        int n = flavor_assets(&all[i], plat, assets);
        int cached = 1;
        for (int j = 0; j < n; j++) {
            char p[PATH_MAX];
            snprintf(p, sizeof(p), "%s/%s", cache_dir, assets[j]);
            if (access(p, R_OK) != 0) { cached = 0; break; }
        }
        fprintf(stdout, "  %-14s %s\n", all[i].name,
                cached ? "installed" : "not installed (hull platform install)");
    }
    return 0;
}

int hl_cmd_platform(int argc, char **argv, const HlCommandEnv *env)
{
    (void)env;
    const char *repo = HL_PLATFORM_DEFAULT_REPO;
    const char *sub = (argc > 1) ? argv[1] : NULL;
    const char *flavor = NULL;
    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "--repo=", 7) == 0) repo = argv[i] + 7;
        else if (argv[i][0] != '-')              flavor = argv[i];
    }

    if (sub && strcmp(sub, "install") == 0) {
        if (!flavor) {
            fprintf(stderr, "usage: hull platform install <flavor> [--repo=ORG/NAME]\n");
            return 1;
        }
        return cmd_install(flavor, repo);
    }
    if (sub && strcmp(sub, "list") == 0) return cmd_list();

    fprintf(stderr, "usage: hull platform install <flavor> | hull platform list\n");
    return sub ? 1 : 0;
}

#endif /* HL_ENABLE_HTTP_CLIENT */
