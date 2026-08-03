/*
 * commands/tools.c — `hull tools <verb>` dispatcher.
 *
 * Sub-commands:
 *   list                       — print registry + install state
 *   list --json                — machine-readable variant (agents, doctor)
 *   install <name>             — download, verify, install one tool
 *   install --all              — install every tool published for this platform
 *   uninstall <name>           — remove an installed tool
 *
 * Trust chain mirrors `hull update` exactly:
 *   1. Fetch GitHub release metadata for the tag matching HL_VERSION
 *      (version coupling — never `latest`, so a tool can't outrun the
 *      hull binary it's meant for).
 *   2. Download `hull.sha256` + `hull.sha256.sig`.
 *   3. Verify the manifest signature with `hl_release_verify_manifest_sig`.
 *   4. Download the tool asset.
 *   5. Persist via hull/blob_store at `~/.hull/blobs/tools/`. The store
 *      verifies the SHA-256 against the manifest entry (passed as
 *      `expected`) AND short-circuits via stat() on re-install of
 *      identical bytes. The on-disk blob is chmod'd 0755.
 *   6. Atomically (re)place the symlink `~/.hull/tools/<name>` →
 *      blob path so existing PATH-style lookups (cap/wasm.c,
 *      build.lua via tool.find_tool) continue to work unchanged.
 *
 * On builds with an all-zeros release pubkey (pre-v0.1.0 placeholders)
 * the signature step is skipped with a visible warning, matching
 * `hull update`'s policy.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/tools.h"
#include "hull/shared/blob_store.h"
#include "hull/shared/cache_registry.h"
#include "hull/release.h"
#include "hull/release_io.h"
#include "hull/tools_install.h"
#include "hull/cap/tar.h"
#include "sh_json.h"

#include <keel/allocator.h>
#include <keel/tls_mbedtls.h>

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef HL_VERSION
#define HL_VERSION "dev"
#endif

#define HL_DEFAULT_REPO "artalis-io/hull"

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Compose the GitHub release tag this hull binary maps to. Tags carry a
 * leading "v" on the GitHub side ("v0.1.2"), but HL_VERSION is typically
 * raw ("0.1.2") or includes a "-dev" suffix in development builds.
 * Returns 0 on success. The tag is also used to pick the right release
 * via api.github.com/.../releases/tags/<tag>. */
static int compose_tag(char *out, size_t out_sz)
{
    if (!out || out_sz < 2) return -1;
    const char *v = HL_VERSION;
    /* Strip any leading "v" so the caller doesn't double up. */
    if (v[0] == 'v') v++;
    int n = snprintf(out, out_sz, "v%s", v);
    if (n < 0 || (size_t)n >= out_sz) return -1;
    return 0;
}

/* Pretty size for human listing. */
static void format_size(size_t bytes, char *out, size_t out_sz)
{
    if (bytes < 1024) {
        snprintf(out, out_sz, "%zu B", bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(out, out_sz, "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(out, out_sz, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    }
}

/* Generic FILE* writer for ShJsonWriter (same shape as in
 * commands/cache.c, sbom.c, commands/doctor.c). */
static int stdio_write_fn(void *ctx, const char *data, size_t len)
{
    FILE *fp = (FILE *)ctx;
    return fwrite(data, 1, len, fp) == len ? 0 : -1;
}

/* Returns 1 if a tool is installed at $HOME/.hull/tools/<name>. The
 * out_path / out_size args are optional and filled when given. */
static int tool_installed(const char *name, char *out_path, size_t out_path_sz,
                          size_t *out_size)
{
    char path[PATH_MAX];
    if (hl_tools_install_path(name, path, sizeof(path)) != 0) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;

    const HlToolSpec *spec = hl_tools_find(name);
    if (spec && spec->is_bundle) {
        /* A bundle installs as a DIRECTORY; consider it present when the dir
         * exists and holds its bundle_entry sentinel (crt1.o for the floor, the
         * zig/lld executable for a toolchain - a complete extraction). */
        if (!S_ISDIR(st.st_mode) || !spec->bundle_entry) return 0;
        char sentinel[PATH_MAX];
        int n = snprintf(sentinel, sizeof(sentinel), "%s/%s", path,
                         spec->bundle_entry);
        struct stat cs;
        if (n < 0 || (size_t)n >= sizeof(sentinel) ||
            stat(sentinel, &cs) != 0 || !S_ISREG(cs.st_mode))
            return 0;
        if (out_path && out_path_sz > 0) snprintf(out_path, out_path_sz, "%s", path);
        if (out_size) *out_size = 0;
        return 1;
    }

    if (!S_ISREG(st.st_mode)) return 0;
    if (out_path && out_path_sz > 0)
        snprintf(out_path, out_path_sz, "%s", path);
    if (out_size) *out_size = (size_t)st.st_size;
    return 1;
}

/* ── `hull tools list` ───────────────────────────────────────────── */

static int print_list_text(const char *platform)
{
    const char *version = HL_VERSION;
    if (version[0] == 'v') version++;
    fprintf(stdout, "Available tools for hull %s on %s:\n\n",
            version, platform);

    int any = 0;
    for (const HlToolSpec *t = hl_tools_registry(); t->name; t++) {
        any = 1;
        int published = hl_tools_published_for(t, platform);
        char inst_path[PATH_MAX];
        size_t inst_size = 0;
        int installed = tool_installed(t->name,
                                       inst_path, sizeof(inst_path),
                                       &inst_size);

        if (installed) {
            char sz_buf[32];
            format_size(inst_size, sz_buf, sizeof(sz_buf));
            fprintf(stdout, "  %-10s [installed]  %s at %s\n",
                    t->name, sz_buf, inst_path);
        } else if (published) {
            fprintf(stdout, "  %-10s [available]  hint: `hull tools install %s`\n",
                    t->name, t->name);
        } else {
            fprintf(stdout, "  %-10s [unavailable for %s]\n",
                    t->name, platform);
        }
        fprintf(stdout, "             %s\n\n", t->description);
    }
    if (!any) fprintf(stdout, "  (no tools registered)\n");

    fprintf(stdout, "Run `hull tools install <name>` to install. "
                    "`--all` to install everything.\n");
    return 0;
}

static int print_list_json(const char *platform)
{
    const char *version = HL_VERSION;
    if (version[0] == 'v') version++;

    ShJsonWriter w;
    sh_json_writer_init(&w, stdio_write_fn, stdout);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "hull_version", version);
    sh_json_write_kv_string(&w, "platform",     platform);
    sh_json_write_key      (&w, "tools");
    sh_json_write_array_start(&w);

    for (const HlToolSpec *t = hl_tools_registry(); t->name; t++) {
        sh_json_write_object_start(&w);
        sh_json_write_kv_string(&w, "name",        t->name);
        sh_json_write_kv_string(&w, "description", t->description);

        int published = hl_tools_published_for(t, platform);
        sh_json_write_kv_bool(&w, "available", published != 0);

        char inst_path[PATH_MAX];
        size_t inst_size = 0;
        int installed = tool_installed(t->name,
                                       inst_path, sizeof(inst_path),
                                       &inst_size);
        sh_json_write_kv_bool(&w, "installed", installed != 0);
        if (installed) {
            sh_json_write_kv_string(&w, "path",       inst_path);
            sh_json_write_kv_int   (&w, "size_bytes", (int64_t)inst_size);
        }
        if (published) {
            char asset[128];
            hl_tools_asset_name(t, platform, asset, sizeof(asset));
            sh_json_write_kv_string(&w, "asset_name", asset);
        }
        sh_json_write_object_end(&w);
    }
    sh_json_write_array_end(&w);
    sh_json_write_object_end(&w);
    fputc('\n', stdout);
    return 0;
}

/* ── `hull tools install` ────────────────────────────────────────── */

/* The verified-manifest fetch (download hull.sha256 + .sig, verify the
 * Ed25519 signature) is the shared trust chain in release_io.c, used by
 * both `hull tools install` and `hull flavor install`:
 *   hl_release_io_fetch_verified_manifest(repo, tag, alloc, tls,
 *                                         "hull-tools", &manifest, &len). */

static int install_one(const HlToolSpec *spec, const char *platform,
                       const char *repo, const char *tag,
                       KlAllocator *alloc, KlTlsCtx *tls,
                       const char *manifest, size_t manifest_len)
{
    if (!hl_tools_published_for(spec, platform)) {
        fprintf(stderr,
                "hull tools: no %s binary published for %s on this hull release.\n",
                spec->name, platform);
        if (strcmp(platform, "cosmo") == 0 && strcmp(spec->name, "wamrc") == 0) {
            fprintf(stderr,
                    "             cosmo users build wamrc from source: `make wamrc`.\n");
        }
        return -1;
    }

    char asset[128];
    if (hl_tools_asset_name(spec, platform, asset, sizeof(asset)) != 0) {
        fprintf(stderr, "hull tools: failed to compose asset name\n");
        return -1;
    }

    /* Locate this asset in the verified manifest BEFORE downloading. */
    char expected[65];
    if (hl_release_io_find_checksum(manifest, manifest_len,
                                    asset, expected) != 0) {
        fprintf(stderr, "hull tools: no checksum entry for %s in hull.sha256\n",
                asset);
        return -1;
    }

    /* Download. */
    char asset_url[256];
    snprintf(asset_url, sizeof(asset_url),
             "https://github.com/%s/releases/download/%s/%s",
             repo, tag, asset);
    fprintf(stdout, "hull tools: downloading %s …\n", asset);

    char *body = NULL;
    size_t body_len = 0;
    if (hl_release_io_get(asset_url, &body, &body_len, alloc, tls,
                          "hull-tools") != 0) {
        fprintf(stderr, "hull tools: failed to download %s\n", asset_url);
        return -1;
    }
    fprintf(stdout, "hull tools: downloaded %zu bytes\n", body_len);

    /* ── Persist via hull/blob_store (CAS mode) ──────────────────────
     *
     * Tool binaries live at $HOME/.hull/blobs/tools/blobs/<XX>/<sha>
     * — the same content-addressed layout that apps' blob stores +
     * runtime caches use. We pass `expected` (the signed SHA from the
     * verified manifest) so blob_store does the SHA verification AND
     * the put_verified short-circuit for us:
     *
     *   - First install: writes body to a tmp file, hashes during
     *     the write (no buffered double-pass), atomic-renames to
     *     blobs/<XX>/<sha>, fsyncs the dirent (put_durable).
     *   - Re-install of bytes that already exist on disk: stat-only
     *     fast path. No write, no SHA recompute.
     *   - SHA mismatch: blob_store fails closed and unlinks the tmp;
     *     we report it and abort. Same security envelope as before.
     *
     * The user-visible name (~/.hull/tools/<name>) becomes a symlink
     * to the blob — preserves the existing PATH-style lookup
     * (`hl_tools_lookup_path` uses `access(X_OK)` which follows
     * symlinks transparently) while letting multiple tool versions
     * coexist in the CAS pool and dedup against `hull update`'s
     * future use of the same store. */

    /* Resolve the tools-store root via the cache registry — same
     * source of truth as `hull cache list|verify`, `hull doctor`,
     * and `hull inspect`, so a layout change in one place reaches
     * all consumers. */
    char blobs_root[PATH_MAX];
    {
        const HlCacheKind *tools_kind = hl_cache_find("tools");
        if (!tools_kind ||
            hl_cache_resolve_path(tools_kind,
                                  blobs_root, sizeof(blobs_root)) != 0) {
            fprintf(stderr, "hull tools: cannot resolve tools-store path "
                            "(HOME unset or registry missing)\n");
            kl_free(alloc, body, body_len);
            return -1;
        }
    }

    HlBlobStore *store = NULL;
    if (hl_blob_store_open(&store, NULL, blobs_root,
                           /*shard_depth=*/1, 0) != 0) {
        fprintf(stderr, "hull tools: cannot open blob store at %s: %s\n",
                blobs_root, strerror(errno));
        kl_free(alloc, body, body_len);
        return -1;
    }

    char blob_id[65];
    if (hl_blob_store_put_durable(store, (const uint8_t *)body, body_len,
                                  expected, blob_id) != 0) {
        fprintf(stderr,
                "hull tools: SHA-256 mismatch or write failure for %s "
                "(expected %s)\n", asset, expected);
        hl_blob_store_close(store);
        kl_free(alloc, body, body_len);
        return -1;
    }
    fprintf(stdout, "hull tools: SHA-256 verified, blob stored (%s)\n",
            blob_id);

    /* A bundle (libc-musl-<arch>) is a .tar of several files, not one
     * executable: extract the just-verified archive into ~/.hull/tools/<name>/
     * and we're done - no blob chmod / symlink dance. */
    if (spec->is_bundle) {
        char dest[PATH_MAX];
        int rc = hl_tools_install_path(spec->name, dest, sizeof(dest));
        if (rc == 0)
            rc = hl_tar_extract((const unsigned char *)body, body_len, dest);
        hl_blob_store_close(store);
        kl_free(alloc, body, body_len);
        if (rc != 0) {
            fprintf(stderr, "hull tools: failed to extract %s bundle\n", spec->name);
            return -1;
        }
        fprintf(stdout, "hull tools: installed %s → %s/ (bundle)\n",
                spec->name, dest);
        return 0;
    }

    kl_free(alloc, body, body_len);

    /* Compose the on-disk blob path so we can chmod it executable and
     * symlink to it. Routing through hl_blob_store_compose_path keeps
     * shard-layout knowledge inside blob_store — see the helper's
     * docstring in include/hull/blob_store.h. */
    char blob_path[PATH_MAX];
    if (hl_blob_store_compose_path(store, blob_id,
                                   blob_path, sizeof(blob_path)) != 0) {
        fprintf(stderr, "hull tools: blob path overflow\n");
        hl_blob_store_close(store);
        return -1;
    }

    /* Tools need to be exec(2)-able. The blob layer writes 0644 by
     * design (apps' content blobs aren't executable); we widen
     * permissions here because we KNOW this blob is a tool. Sharing
     * a blob across multiple tools is fine — all the symlinks point
     * at the same exec-capable file. */
    if (chmod(blob_path, 0755) != 0) {
        fprintf(stderr,
                "hull tools: warning: chmod 0755 failed for %s: %s\n",
                blob_path, strerror(errno));
        /* Non-fatal — the symlink might still resolve if the blob
         * was already executable from a prior install of the same
         * bytes. */
    }
    hl_blob_store_close(store);

    /* Atomically (re)place the user-visible symlink at
     * ~/.hull/tools/<name> → <blob_path>. symlink(2) doesn't
     * overwrite, so we go via a per-pid tmp + rename(2) — that gives
     * us the same atomicity guarantee blob_store gives for blobs. */
    char tools_dir[PATH_MAX];
    if (hl_tools_dir(tools_dir, sizeof(tools_dir)) != 0) {
        fprintf(stderr, "hull tools: cannot create ~/.hull/tools: %s\n",
                strerror(errno));
        return -1;
    }
    char target[PATH_MAX];
    if (hl_tools_install_path(spec->name, target, sizeof(target)) != 0) {
        fprintf(stderr, "hull tools: cannot compose install path\n");
        return -1;
    }

    char tmp_link[PATH_MAX];
    int n = snprintf(tmp_link, sizeof(tmp_link), "%s.tmp.%d", target, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp_link)) {
        fprintf(stderr, "hull tools: symlink tmp path overflow\n");
        return -1;
    }
    (void)unlink(tmp_link);                /* clear any stale tmp */
    if (symlink(blob_path, tmp_link) != 0) {
        fprintf(stderr, "hull tools: symlink %s → %s failed: %s\n",
                tmp_link, blob_path, strerror(errno));
        return -1;
    }
    if (rename(tmp_link, target) != 0) {
        fprintf(stderr, "hull tools: rename %s → %s failed: %s\n",
                tmp_link, target, strerror(errno));
        (void)unlink(tmp_link);
        return -1;
    }

    fprintf(stdout, "hull tools: installed %s → %s (blob %.12s…)\n",
            spec->name, target, blob_id);
    return 0;
}

static int cmd_install(int argc, char **argv, const char *repo)
{
    int install_all = 0;
    const char *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--all") == 0)            install_all = 1;
        else if (strncmp(argv[i], "--repo=", 7) == 0) repo = argv[i] + 7;
        else if (argv[i][0] != '-')                   name = argv[i];
    }
    if (!install_all && !name) {
        fprintf(stderr,
                "Usage: hull tools install <name>\n"
                "       hull tools install --all\n");
        return 2;
    }

    /* Fail fast on unknown names — don't make a network round-trip for
     * something that's not in the registry. */
    if (!install_all && !hl_tools_find(name)) {
        fprintf(stderr,
                "hull tools: unknown tool '%s'. Run `hull tools list` for the registry.\n",
                name);
        return 1;
    }

    const char *platform = hl_release_io_platform();

    /* Set up TLS once for all installs in this invocation. */
    KlAllocator alloc = kl_allocator_default();
    KlTlsCtx *tls = hl_release_io_open_tls(&alloc);
    if (!tls) {
        fprintf(stderr, "hull tools: no CA bundle available — cannot verify HTTPS\n");
        return 1;
    }

    /* Compose tag for THIS hull binary's version (not "latest"). */
    char tag[64];
    if (compose_tag(tag, sizeof(tag)) != 0) {
        fprintf(stderr, "hull tools: cannot compose release tag\n");
        kl_tls_mbedtls_ctx_destroy(tls);
        return 1;
    }

    /* Fetch + verify manifest once, reuse for every asset. */
    char *manifest = NULL;
    size_t manifest_len = 0;
    if (hl_release_io_fetch_verified_manifest(repo, tag, &alloc, tls,
                                "hull-tools", &manifest, &manifest_len,
                                NULL, NULL) != 0) {
        kl_tls_mbedtls_ctx_destroy(tls);
        return 1;
    }

    int rc = 0;
    if (install_all) {
        int any_failed = 0;
        for (const HlToolSpec *t = hl_tools_registry(); t->name; t++) {
            if (!hl_tools_published_for(t, platform)) {
                fprintf(stdout, "hull tools: skipping %s (not published for %s)\n",
                        t->name, platform);
                continue;
            }
            if (install_one(t, platform, repo, tag, &alloc, tls,
                            manifest, manifest_len) != 0) {
                any_failed = 1;
            }
        }
        rc = any_failed ? 1 : 0;
    } else {
        const HlToolSpec *spec = hl_tools_find(name);
        if (!spec) {
            fprintf(stderr,
                    "hull tools: unknown tool '%s'. Run `hull tools list` for the registry.\n",
                    name);
            rc = 1;
        } else {
            rc = install_one(spec, platform, repo, tag, &alloc, tls,
                             manifest, manifest_len) == 0 ? 0 : 1;
        }
    }

    kl_free(&alloc, manifest, manifest_len);
    kl_tls_mbedtls_ctx_destroy(tls);
    return rc;
}

/* ── `hull tools uninstall` ──────────────────────────────────────── */

/* Recursively remove a file or directory tree (an extracted bundle, which may
 * be nested - zig's lib/). Depth is bounded by the bundle's real nesting.
 * Returns 0 on success, -1 on any failure (errno set). */
static int rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0 ? 0 : -1;

    DIR *d = opendir(path);
    if (!d) return -1;
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char child[PATH_MAX];
        int n = snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) { rc = -1; continue; }
        if (rm_rf(child) != 0) rc = -1;
    }
    closedir(d);
    if (rmdir(path) != 0) rc = -1;
    return rc;
}

static int cmd_uninstall(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: hull tools uninstall <name>\n");
        return 2;
    }
    const char *name = argv[1];
    const HlToolSpec *spec = hl_tools_find(name);
    if (!spec) {
        fprintf(stderr, "hull tools: unknown tool '%s'\n", name);
        return 1;
    }
    char path[PATH_MAX];
    if (hl_tools_install_path(name, path, sizeof(path)) != 0) {
        fprintf(stderr, "hull tools: cannot compose install path\n");
        return 1;
    }

    /* A bundle installs as a DIRECTORY, possibly nested (zig's lib/ tree).
     * Remove it recursively - nftw(FTW_DEPTH) visits children before parents. */
    if (spec->is_bundle) {
        struct stat bst;
        if (stat(path, &bst) != 0) {
            if (errno == ENOENT) {
                fprintf(stdout, "hull tools: %s is not installed\n", name);
                return 0;
            }
            fprintf(stderr, "hull tools: cannot stat %s: %s\n",
                    path, strerror(errno));
            return 1;
        }
        if (rm_rf(path) != 0) {
            fprintf(stderr, "hull tools: cannot remove %s: %s\n",
                    path, strerror(errno));
            return 1;
        }
        fprintf(stdout, "hull tools: uninstalled %s (%s)\n", name, path);
        return 0;
    }

    if (unlink(path) != 0) {
        if (errno == ENOENT) {
            fprintf(stdout, "hull tools: %s is not installed\n", name);
            return 0;
        }
        fprintf(stderr, "hull tools: cannot remove %s: %s\n", path, strerror(errno));
        return 1;
    }
    fprintf(stdout, "hull tools: uninstalled %s (%s)\n", name, path);
    return 0;
}

/* ── Dispatcher ──────────────────────────────────────────────────── */

static void usage(void)
{
    fprintf(stdout,
        "Usage: hull tools <verb> [args]\n"
        "\n"
        "Verbs:\n"
        "  list [--json]              list registered tools + install state\n"
        "  install <name>             download, verify, install a tool\n"
        "  install --all              install everything for this platform\n"
        "  uninstall <name>           remove an installed tool\n"
        "\n"
        "Global flags:\n"
        "  --repo=ORG/NAME            override GitHub repo (default: " HL_DEFAULT_REPO ")\n"
        "\n"
        "Tools install to $HOME/.hull/tools/. The trust chain is the same\n"
        "Ed25519-signed hull.sha256 manifest that protects `hull update`.\n");
}

int hl_cmd_tools(int argc, char **argv, const HlCommandEnv *env)
{
    (void)env;
    if (argc < 2) {
        usage();
        return 2;
    }

    const char *repo = HL_DEFAULT_REPO;
    /* Pre-scan for --repo so list and install can both honour it. */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--repo=", 7) == 0) repo = argv[i] + 7;
    }

    const char *verb = argv[1];
    if (strcmp(verb, "-h") == 0 || strcmp(verb, "--help") == 0) {
        usage();
        return 0;
    }

    if (strcmp(verb, "list") == 0) {
        int json = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--json") == 0) json = 1;
        }
        const char *platform = hl_release_io_platform();
        return json ? print_list_json(platform) : print_list_text(platform);
    }

    if (strcmp(verb, "install") == 0) {
        return cmd_install(argc - 1, argv + 1, repo);
    }

    if (strcmp(verb, "uninstall") == 0 || strcmp(verb, "remove") == 0) {
        return cmd_uninstall(argc - 1, argv + 1);
    }

    fprintf(stderr, "hull tools: unknown verb '%s'\n", verb);
    usage();
    return 2;
}
