/*
 * commands/feature.c - `hull feature install <name>` / `list` / `uninstall`.
 *
 * Fetch signed per-feature libraries (libhull_feature-<name>-<arch>.a) from the
 * release matching this hull's version into $HOME/.hull/feature/, so `hull build`
 * can compose a large optional subsystem (e.g. DuckDB) into the app binary
 * without building it from source. A feature is additive and native-only; the
 * feature model + rationale are in docs/features_and_flavors.md.
 *
 * Trust chain (identical to `hull flavor install` / `hull tools install`):
 *   1. hl_release_io_fetch_verified_manifest downloads hull.sha256 +
 *      hull.sha256.sig and verifies the Ed25519 signature against the embedded
 *      release pubkey.
 *   2. For the asset: look up its SHA-256 in the verified manifest, download,
 *      recompute the SHA-256, constant-time compare, atomic-write.
 *
 * Network-only: compiled when HL_ENABLE_HTTP_CLIENT is on (the fetch needs
 * Keel's HTTPS client).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/feature.h"

#ifdef HL_ENABLE_HTTP_CLIENT

#include "hull/release_io.h"

#include <keel/allocator.h>
#include <keel/tls_mbedtls.h>

#include <errno.h>
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

#define HL_FEATURE_DEFAULT_REPO "artalis-io/hull"

/* The composable-feature registry: name + which native platforms publish it.
 * Features are native-only (no cosmo). Adding a feature = one row here plus a
 * matching `make feature-<name>` target + release-pipeline coverage. */
typedef struct {
    const char *name;
    const char *description;
    int has_linux_x86_64;
    int has_linux_aarch64;
    int has_darwin_arm64;
} HlFeatureSpec;

static const HlFeatureSpec FEATURES[] = {
    { "duckdb",   "DuckDB embedded OLAP SQL backend (duckdb://)", 1, 1, 1 },
    { "postgres", "PostgreSQL wire backend (postgres:// / postgresql://)", 1, 1, 1 },
    { "mysql",    "MySQL / MariaDB wire backend (mysql:// / mariadb://)", 1, 1, 1 },
    { "gpu",      "GPU compute via wgpu-native (Vulkan/Metal)",   1, 1, 1 },
    { "tui",      "Terminal UI (hull.tui: full-screen, input, cell-diff)", 1, 1, 1 },
};

static const HlFeatureSpec *feature_find(const char *name)
{
    for (size_t i = 0; i < sizeof FEATURES / sizeof FEATURES[0]; i++)
        if (strcmp(FEATURES[i].name, name) == 0) return &FEATURES[i];
    return NULL;
}

/* Is @p f published for @p plat? Cosmo (and anything non-native) is never
 * published -- features are native-only static archives. */
static int feature_published_for(const HlFeatureSpec *f, const char *plat)
{
    if (strcmp(plat, "linux-x86_64")  == 0) return f->has_linux_x86_64;
    if (strcmp(plat, "linux-aarch64") == 0) return f->has_linux_aarch64;
    if (strcmp(plat, "darwin-arm64")  == 0) return f->has_darwin_arm64;
    return 0;
}

/* libhull_feature-<name>-<platform>.a */
static int feature_asset(const char *name, const char *plat,
                         char *out, size_t out_sz)
{
    int n = snprintf(out, out_sz, "libhull_feature-%s-%s.a", name, plat);
    return (n > 0 && (size_t)n < out_sz) ? 0 : -1;
}

/* GitHub tags carry a leading "v"; HL_VERSION usually does not. */
static int compose_tag(char *out, size_t out_sz)
{
    const char *v = HL_VERSION;
    int n = (v[0] == 'v') ? snprintf(out, out_sz, "%s", v)
                          : snprintf(out, out_sz, "v%s", v);
    return (n > 0 && (size_t)n < out_sz) ? 0 : -1;
}

/* $HOME/.hull/feature, created 0700. */
static int feature_cache_dir(char *out, size_t out_sz)
{
    const char *home = getenv("HOME");
    if (!home || !*home) {
        fprintf(stderr, "hull feature: HOME is not set\n");
        return -1;
    }
    char hull[PATH_MAX];
    if ((size_t)snprintf(hull, sizeof(hull), "%s/.hull", home) >= sizeof(hull))
        return -1;
    if (mkdir(hull, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "hull feature: cannot create %s: %s\n", hull, strerror(errno));
        return -1;
    }
    if ((size_t)snprintf(out, out_sz, "%s/.hull/feature", home) >= out_sz)
        return -1;
    if (mkdir(out, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "hull feature: cannot create %s: %s\n", out, strerror(errno));
        return -1;
    }
    return 0;
}

/* Constant-time compare of two 64-char hex SHA-256 strings (public values;
 * matches the rest of Hull's hash compares). */
static int ct_hex_eq(const char *a, const char *b)
{
    unsigned char diff = 0;
    for (int i = 0; i < 64; i++)
        diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

static int install_asset(const char *asset, const char *repo, const char *tag,
                         KlAllocator *alloc, KlTlsCtx *tls,
                         const char *manifest, size_t manifest_len,
                         const char *cache_dir)
{
    char expected[65];
    if (hl_release_io_find_checksum(manifest, manifest_len, asset, expected) != 0) {
        fprintf(stderr, "hull feature: no checksum entry for %s in hull.sha256 "
                        "(this release may not publish that feature)\n", asset);
        return -1;
    }

    char url[512];
    int un = snprintf(url, sizeof(url),
             "https://github.com/%s/releases/download/%s/%s", repo, tag, asset);
    if (un < 0 || (size_t)un >= sizeof(url)) {
        fprintf(stderr, "hull feature: download URL too long (check --repo)\n");
        return -1;
    }
    fprintf(stdout, "hull feature: downloading %s ...\n", asset);

    char *body = NULL;
    size_t body_len = 0;
    if (hl_release_io_get(url, &body, &body_len, alloc, tls, "hull-feature") != 0) {
        fprintf(stderr, "hull feature: failed to download %s\n", url);
        return -1;
    }

    char actual[65];
    if (hl_release_io_sha256_hex((const unsigned char *)body, body_len, actual) != 0) {
        fprintf(stderr, "hull feature: SHA-256 computation failed\n");
        kl_free(alloc, body, body_len);
        return -1;
    }
    if (!ct_hex_eq(expected, actual)) {
        fprintf(stderr, "hull feature: SHA-256 MISMATCH for %s\n"
                        "  expected %s\n  actual   %s\n", asset, expected, actual);
        kl_free(alloc, body, body_len);
        return -1;
    }

    char dest[PATH_MAX];
    if ((size_t)snprintf(dest, sizeof(dest), "%s/%s", cache_dir, asset) >= sizeof(dest)) {
        fprintf(stderr, "hull feature: cache path overflow\n");
        kl_free(alloc, body, body_len);
        return -1;
    }
    if (hl_release_io_atomic_write(dest, body, body_len, 0644) != 0) {
        fprintf(stderr, "hull feature: failed to write %s\n", dest);
        kl_free(alloc, body, body_len);
        return -1;
    }
    kl_free(alloc, body, body_len);
    fprintf(stdout, "hull feature: installed %s (SHA-256 verified)\n", dest);
    return 0;
}

static int cmd_install(const char *name, const char *repo)
{
    const HlFeatureSpec *f = feature_find(name);
    if (!f) {
        fprintf(stderr, "hull feature: unknown feature '%s' (valid:", name);
        for (size_t i = 0; i < sizeof FEATURES / sizeof FEATURES[0]; i++)
            fprintf(stderr, " %s", FEATURES[i].name);
        fprintf(stderr, ")\n");
        return 1;
    }

    const char *plat = hl_release_io_platform();
    if (!feature_published_for(f, plat)) {
        fprintf(stderr, "hull feature: '%s' is not available for %s "
                        "(features are native-only; not published for cosmo)\n",
                name, plat);
        return 1;
    }

    char cache_dir[PATH_MAX];
    if (feature_cache_dir(cache_dir, sizeof(cache_dir)) != 0) return 1;

    char asset[160];
    if (feature_asset(name, plat, asset, sizeof(asset)) != 0) return 1;

    char tag[128];
    if (compose_tag(tag, sizeof(tag)) != 0) {
        fprintf(stderr, "hull feature: cannot compose release tag from version\n");
        return 1;
    }

    KlAllocator alloc = kl_allocator_default();
    KlTlsCtx *tls = hl_release_io_open_tls(&alloc);
    if (!tls) {
        fprintf(stderr, "hull feature: TLS init failed (no CA bundle available)\n");
        return 1;
    }

    char *manifest = NULL, *sig = NULL;
    size_t manifest_len = 0, sig_len = 0;
    if (hl_release_io_fetch_verified_manifest(repo, tag, &alloc, tls, "hull-feature",
                                              &manifest, &manifest_len,
                                              &sig, &sig_len) != 0) {
        kl_tls_mbedtls_ctx_destroy(tls);
        return 1;
    }

    int rc = install_asset(asset, repo, tag, &alloc, tls,
                           manifest, manifest_len, cache_dir) != 0 ? 1 : 0;

    if (rc == 0) {
        /* Cache the signed manifest so a later `hull build` can re-verify the
         * installed feature lib offline against the embedded release pubkey
         * (closes the install->build TOCTOU, same as flavored builds). */
        char p[PATH_MAX];
        if ((size_t)snprintf(p, sizeof(p), "%s/hull.sha256", cache_dir) < sizeof(p))
            hl_release_io_atomic_write(p, manifest, manifest_len, 0644);
        if (sig && (size_t)snprintf(p, sizeof(p), "%s/hull.sha256.sig",
                                    cache_dir) < sizeof(p))
            hl_release_io_atomic_write(p, sig, sig_len, 0644);
    }

    kl_free(&alloc, manifest, manifest_len);
    if (sig) kl_free(&alloc, sig, sig_len);
    kl_tls_mbedtls_ctx_destroy(tls);

    if (rc == 0)
        fprintf(stdout, "hull feature: %s ready. "
                        "`hull build --with=%s` will compose it.\n", f->name, f->name);
    return rc;
}

static int cmd_list(void)
{
    char cache_dir[PATH_MAX];
    if (feature_cache_dir(cache_dir, sizeof(cache_dir)) != 0) return 1;
    const char *plat = hl_release_io_platform();

    fprintf(stdout, "Features (platform: %s, cache: %s):\n", plat, cache_dir);
    for (size_t i = 0; i < sizeof FEATURES / sizeof FEATURES[0]; i++) {
        const HlFeatureSpec *f = &FEATURES[i];
        const char *status;
        if (!feature_published_for(f, plat)) {
            status = "not available on this platform";
        } else {
            char asset[160], p[PATH_MAX];
            feature_asset(f->name, plat, asset, sizeof(asset));
            snprintf(p, sizeof(p), "%s/%s", cache_dir, asset);
            status = (access(p, R_OK) == 0)
                       ? "installed" : "not installed (hull feature install)";
        }
        fprintf(stdout, "  %-10s %-46s %s\n", f->name, f->description, status);
    }
    return 0;
}

static int cmd_uninstall(const char *name)
{
    const HlFeatureSpec *f = feature_find(name);
    if (!f) {
        fprintf(stderr, "hull feature: unknown feature '%s'\n", name);
        return 1;
    }
    char cache_dir[PATH_MAX];
    if (feature_cache_dir(cache_dir, sizeof(cache_dir)) != 0) return 1;

    char asset[160], p[PATH_MAX];
    if (feature_asset(name, hl_release_io_platform(), asset, sizeof(asset)) != 0)
        return 1;
    if ((size_t)snprintf(p, sizeof(p), "%s/%s", cache_dir, asset) >= sizeof(p))
        return 1;
    if (unlink(p) != 0) {
        if (errno == ENOENT) {
            fprintf(stdout, "hull feature: %s is not installed\n", name);
            return 0;
        }
        fprintf(stderr, "hull feature: cannot remove %s: %s\n", p, strerror(errno));
        return 1;
    }
    fprintf(stdout, "hull feature: removed %s\n", p);
    return 0;
}

int hl_cmd_feature(int argc, char **argv, const HlCommandEnv *env)
{
    (void)env;
    const char *repo = HL_FEATURE_DEFAULT_REPO;
    const char *sub = (argc > 1) ? argv[1] : NULL;
    const char *name = NULL;
    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "--repo=", 7) == 0) repo = argv[i] + 7;
        else if (argv[i][0] != '-')              name = argv[i];
    }

    if (sub && strcmp(sub, "install") == 0) {
        if (!name) {
            fprintf(stderr, "usage: hull feature install <name> [--repo=ORG/NAME]\n");
            return 1;
        }
        return cmd_install(name, repo);
    }
    if (sub && strcmp(sub, "list") == 0) return cmd_list();
    if (sub && strcmp(sub, "uninstall") == 0) {
        if (!name) {
            fprintf(stderr, "usage: hull feature uninstall <name>\n");
            return 1;
        }
        return cmd_uninstall(name);
    }

    fprintf(stderr, "usage: hull feature install <name> | list | uninstall <name>\n");
    return sub ? 1 : 0;
}

#endif /* HL_ENABLE_HTTP_CLIENT */
