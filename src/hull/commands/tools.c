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
#include "hull/blob_store.h"
#include "hull/release.h"
#include "hull/release_io.h"
#include "hull/tools_install.h"

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

/* Minimal JSON string escaper for fields with arbitrary content. Emits
 * raw bytes inside `"..."` with the six escapes RFC 8259 §7 requires —
 * `\"`, `\\`, and the C0 control set encoded as `\b`/`\f`/`\n`/`\r`/`\t`
 * or `\u00XX`. UTF-8 multi-byte sequences pass through unchanged (valid
 * JSON per §8.1). Used for tool descriptions, which today are clean but
 * shouldn't have to stay that way for the output to remain valid JSON. */
static void json_emit_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        switch (c) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\b': fputs("\\b",  f); break;
            case '\f': fputs("\\f",  f); break;
            case '\n': fputs("\\n",  f); break;
            case '\r': fputs("\\r",  f); break;
            case '\t': fputs("\\t",  f); break;
            default:
                if (c < 0x20) fprintf(f, "\\u%04x", c);
                else          fputc((int)c, f);
                break;
        }
    }
    fputc('"', f);
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
    fprintf(stdout, "{\n  \"hull_version\": \"%s\",\n", version);
    fprintf(stdout, "  \"platform\": \"%s\",\n", platform);
    fprintf(stdout, "  \"tools\": [");

    int first = 1;
    for (const HlToolSpec *t = hl_tools_registry(); t->name; t++) {
        if (!first) fprintf(stdout, ",");
        fprintf(stdout, "\n    {\n");
        /* Tool names are `[A-Za-z0-9_-]+` per hl_tools_name_valid, so
         * they need no escaping; descriptions are free-form and DO. */
        fprintf(stdout, "      \"name\": \"%s\",\n", t->name);
        fprintf(stdout, "      \"description\": ");
        json_emit_str(stdout, t->description);
        fprintf(stdout, ",\n");

        int published = hl_tools_published_for(t, platform);
        fprintf(stdout, "      \"available\": %s,\n",
                published ? "true" : "false");

        char inst_path[PATH_MAX];
        size_t inst_size = 0;
        int installed = tool_installed(t->name,
                                       inst_path, sizeof(inst_path),
                                       &inst_size);
        fprintf(stdout, "      \"installed\": %s",
                installed ? "true" : "false");
        if (installed) {
            /* Path may contain spaces or other JSON-sensitive chars on
             * user-controlled $HOME — escape it. */
            fprintf(stdout, ",\n      \"path\": ");
            json_emit_str(stdout, inst_path);
            fprintf(stdout, ",\n      \"size_bytes\": %zu", inst_size);
        }
        if (published) {
            char asset[128];
            hl_tools_asset_name(t, platform, asset, sizeof(asset));
            fprintf(stdout, ",\n      \"asset_name\": \"%s\"", asset);
        }
        fprintf(stdout, "\n    }");
        first = 0;
    }
    fprintf(stdout, "\n  ]\n}\n");
    return 0;
}

/* ── `hull tools install` ────────────────────────────────────────── */

/* Fetch hull.sha256 + signature, verify the signature, return both
 * buffers via out_manifest/out_manifest_len. The signature buffer is
 * freed internally; only the manifest is returned.
 *
 * Owns: allocates *out_manifest. Caller kl_free()s.
 *
 * On HL_RELEASE_PUBKEY_HEX = all-zeros placeholder the signature
 * step is skipped with a warning (matches hull update). */
static int fetch_verified_manifest(const char *repo, const char *tag,
                                   KlAllocator *alloc, KlTlsCtx *tls,
                                   char **out_manifest, size_t *out_manifest_len)
{
    char sha_url[256];
    snprintf(sha_url, sizeof(sha_url),
             "https://github.com/%s/releases/download/%s/hull.sha256",
             repo, tag);

    char *manifest = NULL;
    size_t manifest_len = 0;
    if (hl_release_io_get(sha_url, &manifest, &manifest_len, alloc, tls,
                          "hull-tools") != 0) {
        fprintf(stderr,
                "hull tools: failed to download checksum manifest (%s)\n",
                sha_url);
        return -1;
    }

    if (hl_release_pubkey_configured()) {
        char sig_url[256];
        snprintf(sig_url, sizeof(sig_url),
                 "https://github.com/%s/releases/download/%s/hull.sha256.sig",
                 repo, tag);

        char *sig_hex = NULL;
        size_t sig_len = 0;
        if (hl_release_io_get(sig_url, &sig_hex, &sig_len, alloc, tls,
                              "hull-tools") != 0) {
            fprintf(stderr,
                "hull tools: failed to download release signature (hull.sha256.sig)\n"
                "             — this release is not signed; refusing to install\n");
            kl_free(alloc, manifest, manifest_len);
            return -1;
        }

        int sig_rc = hl_release_verify_manifest_sig(manifest, manifest_len,
                                                    sig_hex, sig_len, NULL);
        kl_free(alloc, sig_hex, sig_len);
        if (sig_rc != 0) {
            fprintf(stderr,
                "hull tools: release signature verification FAILED\n"
                "             — manifest does not match the embedded release public key\n");
            kl_free(alloc, manifest, manifest_len);
            return -1;
        }
        fprintf(stdout, "hull tools: release signature verified\n");
    } else {
        fprintf(stderr,
            "hull tools: WARNING — this hull build has no embedded release public key,\n"
            "             skipping Ed25519 signature check (SHA-256 only).\n");
    }

    *out_manifest = manifest;
    *out_manifest_len = manifest_len;
    return 0;
}

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

    char blobs_root[PATH_MAX];
    {
        const char *home = getenv("HOME");
        if (!home || !*home) {
            fprintf(stderr, "hull tools: HOME not set\n");
            kl_free(alloc, body, body_len);
            return -1;
        }
        int n = snprintf(blobs_root, sizeof(blobs_root),
                         "%s/.hull/blobs/tools", home);
        if (n < 0 || (size_t)n >= sizeof(blobs_root)) {
            fprintf(stderr, "hull tools: blob path overflow\n");
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
    kl_free(alloc, body, body_len);

    /* Compose the on-disk blob path so we can chmod it executable and
     * symlink to it. shard_depth=1 → blobs/<XX>/<id>. Caller-side
     * composition is fine here because the layout is part of the
     * blob_store API contract (documented in include/hull/blob_store.h).
     */
    char blob_path[PATH_MAX];
    int n = snprintf(blob_path, sizeof(blob_path),
                     "%s/blobs/%c%c/%s",
                     blobs_root, blob_id[0], blob_id[1], blob_id);
    if (n < 0 || (size_t)n >= sizeof(blob_path)) {
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
    n = snprintf(tmp_link, sizeof(tmp_link), "%s.tmp.%d", target, (int)getpid());
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
    if (fetch_verified_manifest(repo, tag, &alloc, tls,
                                &manifest, &manifest_len) != 0) {
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

static int cmd_uninstall(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: hull tools uninstall <name>\n");
        return 2;
    }
    const char *name = argv[1];
    if (!hl_tools_find(name)) {
        fprintf(stderr, "hull tools: unknown tool '%s'\n", name);
        return 1;
    }
    char path[PATH_MAX];
    if (hl_tools_install_path(name, path, sizeof(path)) != 0) {
        fprintf(stderr, "hull tools: cannot compose install path\n");
        return 1;
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
