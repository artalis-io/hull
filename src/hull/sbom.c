/*
 * sbom.c. SBOM data table + format implementations.
 *
 * Orthogonal to the rest of the runtime:
 *   - Pure read-only data + format functions.
 *   - No runtime dependencies on db/fs/http/etc. capabilities.
 *   - Optional mbedTLS dependency for SHA-256 of embedded blobs; falls
 *     back gracefully if HL_SBOM_HASH_BLOBS is disabled.
 *
 * Data flow:
 *   Makefile reads submodule SHAs at build time and passes them via
 *   -DHULL_VENDOR_<NAME>_COMMIT="...". For non-submodule snapshots, the
 *   version is hardcoded in the entry table.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/sbom.h"
#include "hull/cacert.h"
#include "sh_json.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#ifndef HL_VERSION
#define HL_VERSION "dev"
#endif

/* Build-time submodule commit + describe-tag defines. Fall back to
 * "unknown" if the Makefile didn't inject them (out-of-tree compile /
 * hand-build). */
#ifndef HULL_VENDOR_KEEL_COMMIT
#define HULL_VENDOR_KEEL_COMMIT "unknown"
#endif
#ifndef HULL_VENDOR_WAMR_COMMIT
#define HULL_VENDOR_WAMR_COMMIT "unknown"
#endif
#ifndef HULL_VENDOR_TCC_COMMIT
#define HULL_VENDOR_TCC_COMMIT "unknown"
#endif
#ifndef HULL_VENDOR_KEEL_VERSION
#define HULL_VENDOR_KEEL_VERSION ""
#endif
#ifndef HULL_VENDOR_WAMR_VERSION
#define HULL_VENDOR_WAMR_VERSION ""
#endif
#ifndef HULL_VENDOR_TCC_VERSION
#define HULL_VENDOR_TCC_VERSION ""
#endif

/* ── SHA-256 of embedded blobs + the running binary (lazy, cached) ──── */

/* SHA-256 algorithm constants. Named for readability; values are fixed
 * by the spec. Used for both digest buffers and hex-encoded outputs. */
#define HL_SHA256_DIGEST_BYTES  32U                          /* 256 bits */
#define HL_SHA256_HEX_LEN       (HL_SHA256_DIGEST_BYTES * 2) /* 64 chars */
#define HL_SHA256_HEX_BUF       (HL_SHA256_HEX_LEN + 1)      /* +1 for NUL */

/* Cap on the size of the binary we'll hash for binary_sha256. The hull
 * binary itself is ~5-15 MB across all build flavors; 256 MB is well
 * past any plausible value and guards against accidentally hashing a
 * giant file when set_binary_path is mis-targeted. */
#define HL_SBOM_BINARY_MAX_BYTES (256U * 1024U * 1024U)

#if defined(HL_EMBED_CA_BUNDLE) || defined(HL_SBOM_HASH_BLOBS)
#define HL_SBOM_HAS_MBEDTLS 1
/* SHA-256 comes from the cap layer's self-contained implementation, not
 * mbedTLS, so the SBOM hashing helpers link in the pure-compute flavor too
 * (mbedTLS is dropped there). The gate name is retained for back-compat; it
 * now just toggles whether these hashing helpers are compiled at all. */
#include "hull/cap/crypto.h"

/* Encode raw SHA-256 bytes as lowercase hex; writes exactly
 * HL_SHA256_HEX_LEN chars plus a NUL terminator into out_hex. */
static void hex_encode_sha256(const unsigned char *raw, char *out_hex)
{
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < HL_SHA256_DIGEST_BYTES; i++) {
        out_hex[i*2]   = hex[(raw[i] >> 4) & 0xf];
        out_hex[i*2+1] = hex[raw[i] & 0xf];
    }
    out_hex[HL_SHA256_HEX_LEN] = '\0';
}

/* Compute SHA-256 over an in-memory buffer; cache the hex result. */
static const char *compute_blob_sha256(const unsigned char *data, size_t len,
                                       char *cache, int *cached)
{
    if (*cached) return cache;
    unsigned char digest[HL_SHA256_DIGEST_BYTES];
    if (hl_cap_crypto_sha256(data, len, digest) != 0) {
        snprintf(cache, HL_SHA256_HEX_BUF, "error");
        *cached = 1;
        return cache;
    }
    hex_encode_sha256(digest, cache);
    *cached = 1;
    return cache;
}
#endif

#ifdef HL_EMBED_CA_BUNDLE
static const char *sha256_ca_bundle(void)
{
    static char cache[HL_SHA256_HEX_BUF] = {0};
    static int cached = 0;
    const unsigned char *data = NULL;
    size_t len = 0;
    if (hl_embedded_ca_bundle(&data, &len) != 0 || !data || len == 0)
        return NULL;
    return compute_blob_sha256(data, len, cache, &cached);
}
#endif

/* ── Binary self-SHA-256 (the running hull binary, lazy + cached) ──── */

/* Caller-provided path (typically argv[0] forwarded by the command
 * dispatcher). The pointer's lifetime is the caller's; we only read it. */
static const char *g_binary_path = NULL;

/* Cache for the binary SHA-256 result. File-scope (not function-static)
 * so set_binary_path can invalidate it when the path changes; otherwise
 * a first call with no path set would poison the cache for later calls. */
static char g_binary_sha_cache[HL_SHA256_HEX_BUF] = {0};
static int  g_binary_sha_tried = 0;

void hl_sbom_set_binary_path(const char *path)
{
    g_binary_path = path;
    g_binary_sha_cache[0] = '\0';
    g_binary_sha_tried = 0;
}

/* When set, formatters scope output to the libhull embedding surface:
 * subject named "libhull", components filtered to entries with in_libhull. */
static int g_sbom_libhull = 0;

void hl_sbom_set_scope_libhull(int on) { g_sbom_libhull = on ? 1 : 0; }

/* Build-flavor scope (hull build --flavor). NONE = the binary's actual set. */
typedef enum {
    SBOM_FLAVOR_NONE = 0,
    SBOM_FLAVOR_FULL,
    SBOM_FLAVOR_SERVER_ONLY,
    SBOM_FLAVOR_CLIENT_ONLY,
    SBOM_FLAVOR_PURE_COMPUTE,
} SbomFlavor;

static SbomFlavor  g_sbom_flavor      = SBOM_FLAVOR_NONE;
static const char *g_sbom_flavor_name = NULL;   /* static string; for the subject */

int hl_sbom_set_scope_flavor(const char *flavor)
{
    if (!flavor || !flavor[0]) {
        g_sbom_flavor = SBOM_FLAVOR_NONE;
        g_sbom_flavor_name = NULL;
        return 0;
    }
    static const struct { const char *name; SbomFlavor f; } tbl[] = {
        { "full",         SBOM_FLAVOR_FULL },
        { "server-only",  SBOM_FLAVOR_SERVER_ONLY },
        { "client-only",  SBOM_FLAVOR_CLIENT_ONLY },
        { "pure-compute", SBOM_FLAVOR_PURE_COMPUTE },
    };
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (strcmp(flavor, tbl[i].name) == 0) {
            g_sbom_flavor = tbl[i].f;
            g_sbom_flavor_name = tbl[i].name;
            return 0;
        }
    }
    return -1;
}

/* Subject component name for the current scope. Static-buffer for the flavor
 * form; single-threaded CLI use, like the rest of the sbom module. */
static const char *sbom_subject_name(void)
{
    if (g_sbom_libhull) return "libhull";
    if (g_sbom_flavor != SBOM_FLAVOR_NONE && g_sbom_flavor_name) {
        static char buf[48];
        snprintf(buf, sizeof(buf), "hull (%s)", g_sbom_flavor_name);
        return buf;
    }
    return "hull";
}

/* True if entry @e should be emitted under the current scope. libhull scope
 * drops runtime-only components; the pure-compute flavor drops needs_http
 * ones (Keel + mbedTLS). */
static int sbom_entry_visible(const HlSbomEntry *e)
{
    if (g_sbom_libhull && !e->in_libhull) return 0;
    if (g_sbom_flavor == SBOM_FLAVOR_PURE_COMPUTE && e->needs_http) return 0;
    return 1;
}

/* True when any non-default scope (libhull or a flavor) is active. Such an
 * SBOM describes an archive/flavor, not the running binary, so the binary
 * hash is omitted. */
static int sbom_scoped(void)
{
    return g_sbom_libhull || g_sbom_flavor != SBOM_FLAVOR_NONE;
}

#ifdef HL_SBOM_HAS_MBEDTLS
/* Read the file at g_binary_path into a buffer and hash via the one-shot
 * cap-layer SHA-256 (hl_cap_crypto_sha256, same primitive used by
 * release_io's sha256_hex and by compute_blob_sha256 above).
 * Returns a cached static hex string, or NULL if the path is unset, the
 * file can't be opened, exceeds the size cap, or any I/O step fails.
 *
 * Whole-file read rather than chunked streaming: ~15 MB of malloc for the
 * binary is trivially OK, and the one-shot API is well-trodden across Hull.
 * The HL_SBOM_BINARY_MAX_BYTES cap prevents accidental runaway allocation
 * if the path is mis-targeted at a giant file. */
static const char *sha256_binary(void)
{
    if (g_binary_sha_tried) return g_binary_sha_cache[0] ? g_binary_sha_cache : NULL;
    g_binary_sha_tried = 1;

    if (!g_binary_path) return NULL;
    FILE *fp = fopen(g_binary_path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0 || (size_t)sz > HL_SBOM_BINARY_MAX_BYTES) {
        fclose(fp); return NULL;
    }
    rewind(fp);
    unsigned char *buf = malloc((size_t)sz);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) { free(buf); return NULL; }

    unsigned char digest[HL_SHA256_DIGEST_BYTES];
    int rc = hl_cap_crypto_sha256(buf, got, digest);
    free(buf);
    if (rc != 0) return NULL;
    hex_encode_sha256(digest, g_binary_sha_cache);
    return g_binary_sha_cache;
}
#else
/* Compute-only builds without mbedTLS: no binary hashing available.
 * The `binary_sha256` field is simply omitted from format output. */
static const char *sha256_binary(void) { return NULL; }
#endif

/* ── Static entry table ────────────────────────────────────────────── */

static const HlSbomEntry sbom_entries[] = {
    /* Hull itself */
    {
        .name = "hull",
        .version = HL_VERSION,
        .commit = "",
        .license_spdx = "AGPL-3.0-or-later",
        .url = "https://github.com/artalis-io/hull",
        .role = "this runtime",
        .embedded_blob_sha256 = NULL,
    },

#if defined(HL_ENABLE_HTTP)
    /* ── Submodule: HTTP server library (own project) ──
     * Linked whenever either HTTP half is on; the pure-compute flavor
     * (HL_ENABLE_HTTP=0) fully unlinks Keel (poll backend replaces it), so
     * gate the SBOM entry on the same macro. */
    {
        .name = "keel",
        .in_libhull = 1,
        .needs_http = 1,
        .version = HULL_VENDOR_KEEL_VERSION,
        .commit = HULL_VENDOR_KEEL_COMMIT,
        .license_spdx = "MIT",
        .url = "https://github.com/artalis-io/keel",
        .role = "HTTP/WS server, event loop, async, thread pool",
        .embedded_blob_sha256 = NULL,
    },
#endif

#ifdef HL_ENABLE_LUA
    /* ── Snapshot: Lua 5.4 ── */
    {
        .name = "lua",
        .version = "5.4",
        .commit = "",
        .license_spdx = "MIT",
        .url = "https://www.lua.org/",
        .role = "application scripting runtime",
        .cpe = "cpe:2.3:a:lua:lua:5.4:*:*:*:*:*:*:*",
        .embedded_blob_sha256 = NULL,
    },
#endif

#ifdef HL_ENABLE_JS
    /* ── Snapshot: QuickJS ── */
    {
        .name = "quickjs",
        .version = "2024-01-13",
        .commit = "",
        .license_spdx = "MIT",
        .url = "https://bellard.org/quickjs/",
        .role = "JavaScript (ES2023) runtime",
        .cpe = "cpe:2.3:a:bellard:quickjs:*:*:*:*:*:*:*:*",
        .embedded_blob_sha256 = NULL,
    },
#endif

#ifdef HL_ENABLE_DB
    /* ── Snapshot: SQLite ── */
    {
        .name = "sqlite",
        .in_libhull = 1,
        .version = "3.x",
        .commit = "",
        .license_spdx = "blessing",   /* SQLite uses "Public Domain" / blessing */
        .url = "https://sqlite.org/",
        .role = "embedded database",
        .cpe = "cpe:2.3:a:sqlite:sqlite:*:*:*:*:*:*:*:*",
        .embedded_blob_sha256 = NULL,
    },
#endif

#if defined(HL_ENABLE_HTTP)
    /* ── Snapshot: mbedTLS ──
     * Linked whenever either HTTP half is on (Keel ships both); the
     * pure-compute flavor drops it entirely (crypto falls back to the
     * in-tree SHA + TweetNaCl), so gate the SBOM entry to match. */
    {
        .name = "mbedtls",
        .in_libhull = 1,
        .needs_http = 1,
        .version = "3.x",
        .commit = "",
        .license_spdx = "Apache-2.0",
        .url = "https://github.com/Mbed-TLS/mbedtls",
        .role = "TLS client + crypto primitives",
        .cpe = "cpe:2.3:a:arm:mbed_tls:*:*:*:*:*:*:*:*",
        .embedded_blob_sha256 = NULL,
    },
#endif

    /* ── Snapshot: TweetNaCl ── */
    {
        .name = "tweetnacl",
        .in_libhull = 1,
        .version = "20140427",
        .commit = "",
        .license_spdx = "blessing",   /* public domain */
        .url = "https://tweetnacl.cr.yp.to/",
        .role = "Ed25519 + secretbox + Curve25519",
        .embedded_blob_sha256 = NULL,
    },

    /* ── Snapshot: jart/pledge polyfill ── */
    {
        .name = "pledge",
        .in_libhull = 1,
        .version = "",
        .commit = "",
        .license_spdx = "ISC",
        .url = "https://github.com/jart/pledge",
        .role = "pledge/unveil polyfill (Linux)",
        .embedded_blob_sha256 = NULL,
    },

    /* ── Snapshot: log.c ── */
    {
        .name = "log.c",
        .in_libhull = 1,
        .version = "",
        .commit = "",
        .license_spdx = "MIT",
        .url = "https://github.com/rxi/log.c",
        .role = "structured logging",
        .embedded_blob_sha256 = NULL,
    },

    /* ── Snapshot: sh_arena ── */
    {
        .name = "sh_arena",
        .in_libhull = 1,
        .version = "",
        .commit = "",
        .license_spdx = "MIT",
        .url = "https://github.com/sailorhg/sh_arena",
        .role = "arena allocator",
        .embedded_blob_sha256 = NULL,
    },

    /* ── Snapshot: sh_json ── */
    {
        .name = "sh_json",
        .in_libhull = 1,
        .version = "",
        .commit = "",
        .license_spdx = "MIT",
        .url = "https://github.com/sailorhg/sh_json",
        .role = "streaming JSON writer / parser",
        .embedded_blob_sha256 = NULL,
    },

    /* ── Snapshot: miniz ── */
    {
        .name = "miniz",
        .in_libhull = 1,
        .version = "",
        .commit = "",
        .license_spdx = "MIT",
        .url = "https://github.com/richgel999/miniz",
        .role = "gzip / deflate",
        .embedded_blob_sha256 = NULL,
    },

#ifdef HL_ENABLE_WASM
    /* ── Submodule: WAMR ── */
    {
        .name = "wamr",
        .in_libhull = 1,
        .version = HULL_VENDOR_WAMR_VERSION,
        .commit = HULL_VENDOR_WAMR_COMMIT,
        .license_spdx = "Apache-2.0",
        .url = "https://github.com/bytecodealliance/wasm-micro-runtime",
        .role = "WebAssembly Micro Runtime (compute plugins)",
        .embedded_blob_sha256 = NULL,
    },
#endif

#ifdef HL_ENABLE_GPU
    /* ── Snapshot: wgpu-native ── */
    {
        .name = "wgpu-native",
        .in_libhull = 1,
        .version = "",
        .commit = "",
        .license_spdx = "MPL-2.0 OR Apache-2.0",
        .url = "https://github.com/gfx-rs/wgpu-native",
        .role = "GPU compute (Vulkan/Metal/DX12)",
        .embedded_blob_sha256 = NULL,
    },
#endif

#ifdef HL_ENABLE_TCC
    /* ── Submodule: TinyCC ── */
    {
        .name = "tinycc",
        .version = HULL_VENDOR_TCC_VERSION,
        .commit = HULL_VENDOR_TCC_COMMIT,
        .license_spdx = "LGPL-2.1-or-later",
        .url = "https://github.com/TinyCC/tinycc",
        .role = "embedded C compiler for `hull build`",
        .cpe = "cpe:2.3:a:tinycc:tinycc:*:*:*:*:*:*:*:*",
        .embedded_blob_sha256 = NULL,
    },
#endif

    /* ── Snapshot: stb (image codecs) ── */
    {
        .name = "stb",
        .in_libhull = 1,
        .version = "",
        .commit = "",
        .license_spdx = "MIT OR blessing",  /* dual-licensed */
        .url = "https://github.com/nothings/stb",
        .role = "image decode/encode (PNG/JPEG/BMP)",
        .embedded_blob_sha256 = NULL,
    },

#ifdef HL_EMBED_CA_BUNDLE
    /* ── Embedded blob: Mozilla CA bundle ── */
    {
        .name = "mozilla-ca-bundle",
        .in_libhull = 1,
        .version = "",   /* upstream is dated; date lives in the PEM */
        .commit = "",
        .license_spdx = "MPL-2.0",
        .url = "https://curl.se/docs/caextract.html",
        .role = "embedded HTTPS trust store (~145 roots)",
        .embedded_blob_sha256 = sha256_ca_bundle,
    },
#endif
};

static const size_t sbom_entries_count =
    sizeof(sbom_entries) / sizeof(sbom_entries[0]);

/* Count of entries visible under the current scope (see sbom_entry_visible). */
static size_t sbom_visible_count(void)
{
    size_t n = 0;
    for (size_t i = 0; i < sbom_entries_count; i++)
        if (sbom_entry_visible(&sbom_entries[i])) n++;
    return n;
}

const HlSbomEntry *hl_sbom_entries(size_t *count)
{
    if (count) *count = sbom_entries_count;
    return sbom_entries;
}

int hl_sbom_parse_format(const char *str)
{
    if (!str) return -1;
    if (strcmp(str, "human") == 0)     return HL_SBOM_HUMAN;
    if (strcmp(str, "json") == 0)      return HL_SBOM_JSON;
    if (strcmp(str, "cyclonedx") == 0) return HL_SBOM_CYCLONEDX;
    if (strcmp(str, "spdx") == 0)      return HL_SBOM_SPDX;
    return -1;
}

/* ── Format: human (table) ─────────────────────────────────────────── */

static void format_human(FILE *fp)
{
    if (g_sbom_libhull) {
        fprintf(fp, "libhull SBOM\n");
        fprintf(fp, "libhull (Hull %s), plus %zu component(s):\n",
                HL_VERSION, sbom_visible_count());
    } else if (g_sbom_flavor != SBOM_FLAVOR_NONE) {
        fprintf(fp, "%s SBOM\n", sbom_subject_name());
        fprintf(fp, "Hull %s (%s flavor), plus %zu component(s):\n",
                HL_VERSION, g_sbom_flavor_name, sbom_visible_count());
    } else {
        fprintf(fp, "Hull SBOM\n");
        fprintf(fp, "Hull %s, plus %zu component(s):\n",
                HL_VERSION, sbom_entries_count - 1);
    }
    const char *bin_sha = sbom_scoped() ? NULL : sha256_binary();
    if (bin_sha) fprintf(fp, "Binary sha256: %s\n", bin_sha);
    fputc('\n', fp);

    /* Column widths picked to match the LICENSING.md table aesthetic. */
    fprintf(fp, "  %-20s %-20s %-22s %s\n",
            "name", "version/commit", "license (SPDX)", "role");
    fprintf(fp, "  %-20s %-20s %-22s %s\n",
            "--------------------",
            "--------------------",
            "----------------------",
            "----------------------------------------");

    for (size_t i = 0; i < sbom_entries_count; i++) {
        const HlSbomEntry *e = &sbom_entries[i];
        if (!sbom_entry_visible(e)) continue;
        char vc[40];
        /* Prefer a tagged version string over the raw commit SHA — it
         * reads better in human output. Falls back to commit if no
         * version is set, then "n/a". Matches the precedence used by
         * the CycloneDX and SPDX formatters. */
        if (e->version && e->version[0]) {
            snprintf(vc, sizeof(vc), "%s", e->version);
        } else if (e->commit && e->commit[0]) {
            snprintf(vc, sizeof(vc), "%.12s", e->commit);
        } else {
            snprintf(vc, sizeof(vc), "n/a");
        }
        fprintf(fp, "  %-20s %-20s %-22s %s\n",
                e->name, vc, e->license_spdx, e->role);
        if (e->embedded_blob_sha256) {
            const char *sha = e->embedded_blob_sha256();
            if (sha) fprintf(fp, "  %-20s sha256: %s\n", "", sha);
        }
    }
    fprintf(fp, "\nUrls (full):\n");
    for (size_t i = 0; i < sbom_entries_count; i++) {
        if (!sbom_entry_visible(&sbom_entries[i])) continue;
        fprintf(fp, "  %-20s %s\n", sbom_entries[i].name, sbom_entries[i].url);
    }
}

/* ── JSON writer plumbing ──────────────────────────────────────────── */

/* Generic FILE* writer for ShJsonWriter — same shape as the helper
 * in commands/cache.c. Could be promoted to a shared header if a
 * third caller materializes; for now keep it local. */
static int stdio_write_fn(void *ctx, const char *data, size_t len)
{
    FILE *fp = (FILE *)ctx;
    return fwrite(data, 1, len, fp) == len ? 0 : -1;
}

/* Sanitize an SBOM component name into an SPDX-legal identifier:
 * [A-Za-z0-9.-]+ only; everything else collapses to '-'. Result is
 * written into `out` (caller-provided), NUL-terminated. */
static void spdx_sanitize_name(const char *src, char *out, size_t out_sz)
{
    size_t j = 0;
    for (const char *p = src; *p && j + 1 < out_sz; p++) {
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-')
            out[j++] = c;
        else
            out[j++] = '-';
    }
    out[j] = '\0';
}

/* ── Format: json (flat array, agent-friendly) ─────────────────────── */

static void format_json(FILE *fp)
{
    ShJsonWriter w;
    sh_json_writer_init(&w, stdio_write_fn, fp);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "hull_version", HL_VERSION);
    sh_json_write_kv_string(&w, "subject", sbom_subject_name());
    /* The runtime binary hash describes the hull binary, not libhull.a — omit
     * it in libhull scope (the archive's own hash lives in libhull.a.sha256). */
    const char *bin_sha = sbom_scoped() ? NULL : sha256_binary();
    if (bin_sha)
        sh_json_write_kv_string(&w, "binary_sha256", bin_sha);
    sh_json_write_key(&w, "components");
    sh_json_write_array_start(&w);
    for (size_t i = 0; i < sbom_entries_count; i++) {
        const HlSbomEntry *e = &sbom_entries[i];
        if (!sbom_entry_visible(e)) continue;
        sh_json_write_object_start(&w);
        sh_json_write_kv_string(&w, "name",         e->name);
        sh_json_write_kv_string(&w, "version",      e->version);
        sh_json_write_kv_string(&w, "commit",       e->commit);
        sh_json_write_kv_string(&w, "license_spdx", e->license_spdx);
        sh_json_write_kv_string(&w, "url",          e->url);
        sh_json_write_kv_string(&w, "role",         e->role);
        if (e->cpe && e->cpe[0])
            sh_json_write_kv_string(&w, "cpe", e->cpe);
        if (e->embedded_blob_sha256) {
            const char *sha = e->embedded_blob_sha256();
            sh_json_write_kv_string(&w, "embedded_blob_sha256", sha ? sha : "");
        }
        sh_json_write_object_end(&w);
    }
    sh_json_write_array_end(&w);
    sh_json_write_object_end(&w);
    fputc('\n', fp);
}

/* ── Format: CycloneDX 1.5 ─────────────────────────────────────────── */

static void format_cyclonedx(FILE *fp)
{
    /* Reproducible serialNumber: deterministic, derived from hull version
     * so two runs of `hull sbom --format=cyclonedx` on the same binary
     * produce the same document. */
    ShJsonWriter w;
    sh_json_writer_init(&w, stdio_write_fn, fp);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "bomFormat",   "CycloneDX");
    sh_json_write_kv_string(&w, "specVersion", "1.5");
    sh_json_write_kv_int   (&w, "version",     1);

    /* metadata.component describes the subject of the BOM (this binary).
     * Includes the SHA-256 of the running binary when known, so a consumer
     * can cross-check against the signed hull.sha256 release manifest. */
    const char *bin_sha = sbom_scoped() ? NULL : sha256_binary();
    sh_json_write_key(&w, "metadata");
    sh_json_write_object_start(&w);
    sh_json_write_key(&w, "component");
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "type",    g_sbom_libhull ? "library" : "application");
    sh_json_write_kv_string(&w, "name",    sbom_subject_name());
    sh_json_write_kv_string(&w, "version", HL_VERSION);
    if (bin_sha) {
        sh_json_write_key(&w, "hashes");
        sh_json_write_array_start(&w);
        sh_json_write_object_start(&w);
        sh_json_write_kv_string(&w, "alg",     "SHA-256");
        sh_json_write_kv_string(&w, "content", bin_sha);
        sh_json_write_object_end(&w);
        sh_json_write_array_end(&w);
    }
    sh_json_write_object_end(&w);  /* component */
    sh_json_write_object_end(&w);  /* metadata  */

    sh_json_write_key(&w, "components");
    sh_json_write_array_start(&w);
    for (size_t i = 0; i < sbom_entries_count; i++) {
        const HlSbomEntry *e = &sbom_entries[i];
        if (!sbom_entry_visible(e)) continue;
        sh_json_write_object_start(&w);
        sh_json_write_kv_string(&w, "type", "library");
        sh_json_write_kv_string(&w, "name", e->name);
        if (e->version && e->version[0]) {
            sh_json_write_kv_string(&w, "version", e->version);
        } else if (e->commit && e->commit[0]) {
            /* CycloneDX uses "version" loosely. Commit SHA is fine. */
            sh_json_write_kv_string(&w, "version", e->commit);
        }
        sh_json_write_key(&w, "licenses");
        sh_json_write_array_start(&w);
        sh_json_write_object_start(&w);
        sh_json_write_key(&w, "license");
        sh_json_write_object_start(&w);
        sh_json_write_kv_string(&w, "id", e->license_spdx);
        sh_json_write_object_end(&w);
        sh_json_write_object_end(&w);
        sh_json_write_array_end(&w);

        sh_json_write_key(&w, "externalReferences");
        sh_json_write_array_start(&w);
        sh_json_write_object_start(&w);
        sh_json_write_kv_string(&w, "type", "website");
        sh_json_write_kv_string(&w, "url",  e->url);
        sh_json_write_object_end(&w);
        sh_json_write_array_end(&w);

        /* CycloneDX `cpe` is a top-level component field used by
         * downstream scanners (Dependency-Track, Trivy, etc.) to
         * auto-match against CVE feeds. Emit when known. */
        if (e->cpe && e->cpe[0])
            sh_json_write_kv_string(&w, "cpe", e->cpe);
        if (e->embedded_blob_sha256) {
            const char *sha = e->embedded_blob_sha256();
            if (sha) {
                sh_json_write_key(&w, "hashes");
                sh_json_write_array_start(&w);
                sh_json_write_object_start(&w);
                sh_json_write_kv_string(&w, "alg",     "SHA-256");
                sh_json_write_kv_string(&w, "content", sha);
                sh_json_write_object_end(&w);
                sh_json_write_array_end(&w);
            }
        }
        sh_json_write_kv_string(&w, "description", e->role);
        sh_json_write_object_end(&w);
    }
    sh_json_write_array_end(&w);
    sh_json_write_object_end(&w);
    fputc('\n', fp);
}

/* ── Format: SPDX 2.3 ──────────────────────────────────────────────── */

static void format_spdx(FILE *fp)
{
    /* HL_VERSION is a controlled literal (no chars needing JSON-escape).
     * The "created" timestamp is intentionally stable (not `now()`) so
     * two runs of `hull sbom --format=spdx` on the same binary produce
     * byte-identical output. */
    ShJsonWriter w;
    sh_json_writer_init(&w, stdio_write_fn, fp);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "spdxVersion", "SPDX-2.3");
    sh_json_write_kv_string(&w, "dataLicense", "CC0-1.0");
    sh_json_write_kv_string(&w, "SPDXID",      "SPDXRef-DOCUMENT");
    sh_json_write_kv_string(&w, "name",        "hull-sbom");
    {
        char ns[128];
        snprintf(ns, sizeof(ns), "https://gethull.dev/sbom/%s", HL_VERSION);
        sh_json_write_kv_string(&w, "documentNamespace", ns);
    }
    sh_json_write_key(&w, "creationInfo");
    sh_json_write_object_start(&w);
    sh_json_write_key(&w, "creators");
    sh_json_write_array_start(&w);
    sh_json_write_string(&w, "Tool: hull-sbom");
    sh_json_write_array_end(&w);
    sh_json_write_kv_string(&w, "created", "2026-05-29T00:00:00Z");
    sh_json_write_object_end(&w);

    /* describes + hull-binary package: documents that the subject of this
     * SPDX document is the running hull binary; binary_sha256 is emitted
     * as a checksum on that package so consumers can cross-reference the
     * signed release manifest. */
    const char *bin_sha = sbom_scoped() ? NULL : sha256_binary();
    sh_json_write_key(&w, "documentDescribes");
    sh_json_write_array_start(&w);
    sh_json_write_string(&w, "SPDXRef-Package-hull-binary");
    sh_json_write_array_end(&w);

    sh_json_write_key(&w, "packages");
    sh_json_write_array_start(&w);
    /* The hull-binary package itself. */
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "SPDXID",           "SPDXRef-Package-hull-binary");
    sh_json_write_kv_string(&w, "name",             sbom_subject_name());
    sh_json_write_kv_string(&w, "versionInfo",      HL_VERSION);
    sh_json_write_kv_string(&w, "downloadLocation", "https://github.com/artalis-io/hull");
    sh_json_write_kv_string(&w, "licenseConcluded", "AGPL-3.0-or-later");
    sh_json_write_kv_string(&w, "licenseDeclared",  "AGPL-3.0-or-later");
    sh_json_write_kv_string(&w, "comment",
                            g_sbom_libhull ? "the libhull embedding archive" :
                            g_sbom_flavor != SBOM_FLAVOR_NONE
                                ? "a hull build --flavor platform library"
                                : "the running hull binary");
    if (bin_sha) {
        sh_json_write_key(&w, "checksums");
        sh_json_write_array_start(&w);
        sh_json_write_object_start(&w);
        sh_json_write_kv_string(&w, "algorithm",     "SHA256");
        sh_json_write_kv_string(&w, "checksumValue", bin_sha);
        sh_json_write_object_end(&w);
        sh_json_write_array_end(&w);
    }
    sh_json_write_object_end(&w);

    /* Vendored components. */
    for (size_t i = 0; i < sbom_entries_count; i++) {
        const HlSbomEntry *e = &sbom_entries[i];
        if (!sbom_entry_visible(e)) continue;
        sh_json_write_object_start(&w);
        {
            /* SPDX IDs must match [A-Za-z0-9.-]+; sanitize the name. */
            char spdxid[128];
            char sanitized[96];
            spdx_sanitize_name(e->name, sanitized, sizeof(sanitized));
            snprintf(spdxid, sizeof(spdxid),
                     "SPDXRef-Package-%s", sanitized);
            sh_json_write_kv_string(&w, "SPDXID", spdxid);
        }
        sh_json_write_kv_string(&w, "name", e->name);
        if (e->version && e->version[0])
            sh_json_write_kv_string(&w, "versionInfo", e->version);
        else if (e->commit && e->commit[0])
            sh_json_write_kv_string(&w, "versionInfo", e->commit);
        sh_json_write_kv_string(&w, "downloadLocation", e->url);
        sh_json_write_kv_string(&w, "licenseConcluded", e->license_spdx);
        sh_json_write_kv_string(&w, "licenseDeclared",  e->license_spdx);
        sh_json_write_kv_string(&w, "comment",          e->role);
        if (e->embedded_blob_sha256) {
            const char *sha = e->embedded_blob_sha256();
            if (sha) {
                sh_json_write_key(&w, "checksums");
                sh_json_write_array_start(&w);
                sh_json_write_object_start(&w);
                sh_json_write_kv_string(&w, "algorithm",     "SHA256");
                sh_json_write_kv_string(&w, "checksumValue", sha);
                sh_json_write_object_end(&w);
                sh_json_write_array_end(&w);
            }
        }
        sh_json_write_object_end(&w);
    }
    sh_json_write_array_end(&w);
    sh_json_write_object_end(&w);
    fputc('\n', fp);
}

/* ── Public dispatcher ─────────────────────────────────────────────── */

int hl_sbom_format(HlSbomFormat format, FILE *fp)
{
    if (!fp) return -1;
    switch (format) {
        case HL_SBOM_HUMAN:     format_human(fp);     return 0;
        case HL_SBOM_JSON:      format_json(fp);      return 0;
        case HL_SBOM_CYCLONEDX: format_cyclonedx(fp); return 0;
        case HL_SBOM_SPDX:      format_spdx(fp);      return 0;
    }
    fprintf(stderr, "hull sbom: unknown format %d\n", (int)format);
    return -1;
}
