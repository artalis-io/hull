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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#ifndef HL_VERSION
#define HL_VERSION "dev"
#endif

/* Build-time submodule commit defines. Fall back to "unknown" if the
 * Makefile didn't inject them (out-of-tree compile / hand-build). */
#ifndef HULL_VENDOR_KEEL_COMMIT
#define HULL_VENDOR_KEEL_COMMIT "unknown"
#endif
#ifndef HULL_VENDOR_WAMR_COMMIT
#define HULL_VENDOR_WAMR_COMMIT "unknown"
#endif
#ifndef HULL_VENDOR_TCC_COMMIT
#define HULL_VENDOR_TCC_COMMIT "unknown"
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
#include "mbedtls/sha256.h"

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
    if (mbedtls_sha256(data, len, digest, 0) != 0) {
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

#ifdef HL_SBOM_HAS_MBEDTLS
/* Read the file at g_binary_path into a buffer and hash via the one-shot
 * mbedTLS SHA-256 API (same primitive used by release_io's sha256_hex
 * and by compute_blob_sha256 above; MSan-verified everywhere it's used).
 * Returns a cached static hex string, or NULL if the path is unset, the
 * file can't be opened, exceeds the size cap, or any I/O step fails.
 *
 * Implementation note: we initially used the streaming _starts/_update/
 * _finish API for memory friendliness but MSan-instrumented mbedTLS
 * misreports uninitialized context state through that path. The one-shot
 * API is well-trodden across Hull and ~15 MB of malloc for the binary is
 * trivially OK. The HL_SBOM_BINARY_MAX_BYTES cap prevents accidental
 * runaway allocation if the path is mis-targeted at a giant file. */
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
    int rc = mbedtls_sha256(buf, got, digest, 0);
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

    /* ── Submodule: HTTP server library (own project) ── */
    {
        .name = "keel",
        .version = "",
        .commit = HULL_VENDOR_KEEL_COMMIT,
        .license_spdx = "MIT",
        .url = "https://github.com/artalis-io/keel",
        .role = "HTTP/WS server, event loop, async, thread pool",
        .embedded_blob_sha256 = NULL,
    },

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
        .version = "3.x",
        .commit = "",
        .license_spdx = "blessing",   /* SQLite uses "Public Domain" / blessing */
        .url = "https://sqlite.org/",
        .role = "embedded database",
        .cpe = "cpe:2.3:a:sqlite:sqlite:*:*:*:*:*:*:*:*",
        .embedded_blob_sha256 = NULL,
    },
#endif

    /* ── Snapshot: mbedTLS ── */
    {
        .name = "mbedtls",
        .version = "3.x",
        .commit = "",
        .license_spdx = "Apache-2.0",
        .url = "https://github.com/Mbed-TLS/mbedtls",
        .role = "TLS client + crypto primitives",
        .cpe = "cpe:2.3:a:arm:mbed_tls:*:*:*:*:*:*:*:*",
        .embedded_blob_sha256 = NULL,
    },

    /* ── Snapshot: TweetNaCl ── */
    {
        .name = "tweetnacl",
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
        .version = "",
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
        .version = "",
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
    fprintf(fp, "Hull SBOM\n");
    fprintf(fp, "Hull %s, plus %zu component(s):\n",
            HL_VERSION, sbom_entries_count - 1);
    const char *bin_sha = sha256_binary();
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
        char vc[40];
        if (e->commit && e->commit[0]) {
            snprintf(vc, sizeof(vc), "%.12s", e->commit);
        } else if (e->version && e->version[0]) {
            snprintf(vc, sizeof(vc), "%s", e->version);
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
        fprintf(fp, "  %-20s %s\n", sbom_entries[i].name, sbom_entries[i].url);
    }
}

/* ── JSON helpers (string escape) ──────────────────────────────────── */

static void json_escape(FILE *fp, const char *s)
{
    fputc('"', fp);
    if (!s) { fputc('"', fp); return; }
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"':  fputs("\\\"", fp); break;
            case '\\': fputs("\\\\", fp); break;
            case '\n': fputs("\\n", fp); break;
            case '\r': fputs("\\r", fp); break;
            case '\t': fputs("\\t", fp); break;
            case '\b': fputs("\\b", fp); break;
            case '\f': fputs("\\f", fp); break;
            default:
                if (c < 0x20) fprintf(fp, "\\u%04x", c);
                else fputc(c, fp);
        }
    }
    fputc('"', fp);
}

/* ── Format: json (flat array, agent-friendly) ─────────────────────── */

static void format_json(FILE *fp)
{
    fputs("{\"hull_version\":", fp);
    json_escape(fp, HL_VERSION);
    const char *bin_sha = sha256_binary();
    if (bin_sha) {
        fputs(",\"binary_sha256\":", fp);
        json_escape(fp, bin_sha);
    }
    fputs(",\"components\":[", fp);
    for (size_t i = 0; i < sbom_entries_count; i++) {
        const HlSbomEntry *e = &sbom_entries[i];
        if (i > 0) fputc(',', fp);
        fputc('{', fp);
        fputs("\"name\":", fp); json_escape(fp, e->name);
        fputs(",\"version\":", fp); json_escape(fp, e->version);
        fputs(",\"commit\":", fp); json_escape(fp, e->commit);
        fputs(",\"license_spdx\":", fp); json_escape(fp, e->license_spdx);
        fputs(",\"url\":", fp); json_escape(fp, e->url);
        fputs(",\"role\":", fp); json_escape(fp, e->role);
        if (e->cpe && e->cpe[0]) {
            fputs(",\"cpe\":", fp); json_escape(fp, e->cpe);
        }
        if (e->embedded_blob_sha256) {
            const char *sha = e->embedded_blob_sha256();
            fputs(",\"embedded_blob_sha256\":", fp);
            json_escape(fp, sha ? sha : "");
        }
        fputc('}', fp);
    }
    fputs("]}\n", fp);
}

/* ── Format: CycloneDX 1.5 ─────────────────────────────────────────── */

static void format_cyclonedx(FILE *fp)
{
    /* Reproducible serialNumber: deterministic, derived from hull version
     * so two runs of `hull sbom --format=cyclonedx` on the same binary
     * produce the same document. (Per the same byte-identity goal.) */
    fputs("{\"bomFormat\":\"CycloneDX\",\"specVersion\":\"1.5\","
          "\"version\":1", fp);
    /* metadata.component describes the subject of the BOM (this binary).
     * Includes the SHA-256 of the running binary when known, so a consumer
     * can cross-check against the signed hull.sha256 release manifest. */
    const char *bin_sha = sha256_binary();
    fputs(",\"metadata\":{\"component\":{\"type\":\"application\","
          "\"name\":\"hull\",\"version\":", fp);
    json_escape(fp, HL_VERSION);
    if (bin_sha) {
        fputs(",\"hashes\":[{\"alg\":\"SHA-256\",\"content\":", fp);
        json_escape(fp, bin_sha);
        fputs("}]", fp);
    }
    fputs("}}", fp);
    fputs(",\"components\":[", fp);
    int first = 1;
    for (size_t i = 0; i < sbom_entries_count; i++) {
        const HlSbomEntry *e = &sbom_entries[i];
        if (!first) fputc(',', fp);
        first = 0;
        fputs("{\"type\":\"library\",\"name\":", fp);
        json_escape(fp, e->name);
        if (e->version && e->version[0]) {
            fputs(",\"version\":", fp); json_escape(fp, e->version);
        } else if (e->commit && e->commit[0]) {
            /* CycloneDX uses "version" loosely. Commit SHA is fine. */
            fputs(",\"version\":", fp); json_escape(fp, e->commit);
        }
        fputs(",\"licenses\":[{\"license\":{\"id\":", fp);
        json_escape(fp, e->license_spdx);
        fputs("}}]", fp);
        fputs(",\"externalReferences\":[{\"type\":\"website\",\"url\":", fp);
        json_escape(fp, e->url);
        fputs("}]", fp);
        /* CycloneDX `cpe` is a top-level component field used by
         * downstream scanners (Dependency-Track, Trivy, etc.) to
         * auto-match against CVE feeds. Emit when known. */
        if (e->cpe && e->cpe[0]) {
            fputs(",\"cpe\":", fp); json_escape(fp, e->cpe);
        }
        if (e->embedded_blob_sha256) {
            const char *sha = e->embedded_blob_sha256();
            if (sha) {
                fputs(",\"hashes\":[{\"alg\":\"SHA-256\",\"content\":", fp);
                json_escape(fp, sha);
                fputs("}]", fp);
            }
        }
        fputs(",\"description\":", fp); json_escape(fp, e->role);
        fputc('}', fp);
    }
    fputs("]}\n", fp);
}

/* ── Format: SPDX 2.3 ──────────────────────────────────────────────── */

static void format_spdx(FILE *fp)
{
    /* Namespace is built via preprocessor string concatenation so the
     * URL is a single well-formed JSON string. HL_VERSION is a controlled
     * literal (no chars needing JSON-escape). The "created" timestamp is
     * intentionally stable (not `now()`) so two runs of `hull sbom
     * --format=spdx` on the same binary produce byte-identical output. */
    fputs("{\"spdxVersion\":\"SPDX-2.3\","
          "\"dataLicense\":\"CC0-1.0\","
          "\"SPDXID\":\"SPDXRef-DOCUMENT\","
          "\"name\":\"hull-sbom\","
          "\"documentNamespace\":\"https://gethull.dev/sbom/" HL_VERSION "\","
          "\"creationInfo\":{\"creators\":[\"Tool: hull-sbom\"],"
          "\"created\":\"2026-05-29T00:00:00Z\"}", fp);
    /* describes + hull-binary package: documents that the subject of this
     * SPDX document is the running hull binary; binary_sha256 is emitted
     * as a checksum on that package so consumers can cross-reference the
     * signed release manifest. */
    const char *bin_sha = sha256_binary();
    fputs(",\"documentDescribes\":[\"SPDXRef-Package-hull-binary\"]", fp);
    fputs(",\"packages\":[", fp);
    fputs("{\"SPDXID\":\"SPDXRef-Package-hull-binary\","
          "\"name\":\"hull\",\"versionInfo\":", fp);
    json_escape(fp, HL_VERSION);
    fputs(",\"downloadLocation\":\"https://github.com/artalis-io/hull\","
          "\"licenseConcluded\":\"AGPL-3.0-or-later\","
          "\"licenseDeclared\":\"AGPL-3.0-or-later\","
          "\"comment\":\"the running hull binary\"", fp);
    if (bin_sha) {
        fputs(",\"checksums\":[{\"algorithm\":\"SHA256\",\"checksumValue\":", fp);
        json_escape(fp, bin_sha);
        fputs("}]", fp);
    }
    fputs("},", fp);  /* trailing comma; list of vendored components follows */
    int first = 1;
    for (size_t i = 0; i < sbom_entries_count; i++) {
        const HlSbomEntry *e = &sbom_entries[i];
        if (!first) fputc(',', fp);
        first = 0;
        fputs("{\"SPDXID\":\"SPDXRef-Package-", fp);
        /* SPDX IDs must match [A-Za-z0-9.-]+; sanitize the name. */
        for (const char *p = e->name; *p; p++) {
            char c = *p;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '.' || c == '-')
                fputc(c, fp);
            else
                fputc('-', fp);
        }
        fputs("\",\"name\":", fp); json_escape(fp, e->name);
        if (e->version && e->version[0]) {
            fputs(",\"versionInfo\":", fp); json_escape(fp, e->version);
        } else if (e->commit && e->commit[0]) {
            fputs(",\"versionInfo\":", fp); json_escape(fp, e->commit);
        }
        fputs(",\"downloadLocation\":", fp); json_escape(fp, e->url);
        fputs(",\"licenseConcluded\":", fp); json_escape(fp, e->license_spdx);
        fputs(",\"licenseDeclared\":", fp); json_escape(fp, e->license_spdx);
        fputs(",\"comment\":", fp); json_escape(fp, e->role);
        if (e->embedded_blob_sha256) {
            const char *sha = e->embedded_blob_sha256();
            if (sha) {
                fputs(",\"checksums\":[{\"algorithm\":\"SHA256\",\"checksumValue\":", fp);
                json_escape(fp, sha);
                fputs("}]", fp);
            }
        }
        fputc('}', fp);
    }
    fputs("]}\n", fp);
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
