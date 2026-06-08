/*
 * commands/doctor.c — hull doctor subcommand
 *
 * Checks the local environment and reports whether hull is ready to
 * build, develop, and deploy applications.
 *
 *   hull doctor          — human-readable output
 *   hull doctor --json   — machine-readable JSON
 *
 * Checks performed:
 *   1. Hull binary metadata (version, runtime, platform, build mode)
 *   2. Platform library embedding (none / single-arch / multi-arch)
 *   3. C compiler availability in PATH (cc, gcc, clang, cosmocc)
 *   4. WASM compute capability (HL_ENABLE_WASM, AOT loader, wamrc)
 *   5. GPU compute capability (HL_ENABLE_GPU)
 *   6. Overall hull build readiness summary
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/doctor.h"
#include "hull/blob_store.h"
#include "hull/build_assets.h"
#include "hull/cache_registry.h"
#include "hull/cacert.h"
#include "hull/compiler.h"
#include "hull/module_registry.h"
#include "hull/tool.h"
#include "hull/tools_install.h"
#include "sh_json.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>

#ifndef HL_VERSION
#define HL_VERSION "dev"
#endif

/* ── Runtime / platform / build labels (mirrors version.c) ──────── */

static const char *doctor_runtime(void)
{
#if defined(HL_ENABLE_JS) && defined(HL_ENABLE_LUA)
    return "lua+js";
#elif defined(HL_ENABLE_JS)
    return "js";
#elif defined(HL_ENABLE_LUA)
    return "lua";
#else
    return "none";
#endif
}

static const char *doctor_platform(void)
{
#if defined(__COSMOPOLITAN__)
    return "cosmo";
#elif defined(__APPLE__)
#  if defined(__aarch64__)
    return "darwin-arm64";
#  else
    return "darwin-x86_64";
#  endif
#elif defined(__linux__)
#  if defined(__aarch64__)
    return "linux-arm64";
#  else
    return "linux-x86_64";
#  endif
#else
    return "unknown";
#endif
}

static const char *doctor_build(void)
{
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_MEMORY__)
    return "asan";
#elif defined(DEBUG)
    return "debug";
#else
    return "release";
#endif
}

/* ── Compiler discovery ─────────────────────────────────────────── */

#define MAX_COMPILERS 4

typedef struct {
    const char *name;
    char        path[PATH_MAX];  /* empty string = not found */
} CompilerInfo;

/* Walk PATH and return the first directory containing `name`. */
static int find_in_path(const char *name, char *out, size_t out_sz)
{
    const char *path_env = getenv("PATH");
    if (!path_env || !name || !out || out_sz == 0)
        return 0;

    char buf[4096];
    size_t plen = strlen(path_env);
    if (plen >= sizeof(buf))
        return 0;
    memcpy(buf, path_env, plen + 1);

    char *dir = buf;
    while (dir && *dir) {
        char *colon = strchr(dir, ':');
        if (colon) *colon = '\0';

        char candidate[PATH_MAX];
        int n = snprintf(candidate, sizeof(candidate), "%s/%s", dir, name);
        if (n > 0 && (size_t)n < sizeof(candidate)) {
            if (access(candidate, X_OK) == 0) {
                snprintf(out, out_sz, "%s", candidate);
                return 1;
            }
        }

        dir = colon ? colon + 1 : NULL;
    }
    return 0;
}

static void discover_compilers(CompilerInfo *ci, int count)
{
    for (int i = 0; i < count; i++)
        ci[i].path[0] = '\0';

    for (int i = 0; i < count; i++)
        find_in_path(ci[i].name, ci[i].path, sizeof(ci[i].path));
}

/* ── Platform embedding detection ──────────────────────────────── */

typedef enum {
    PLATFORM_NONE,
    PLATFORM_SINGLE,
    PLATFORM_MULTI
} PlatformEmbed;

static PlatformEmbed detect_platform(void)
{
    if (hl_build_get_platforms(NULL) > 0)
        return PLATFORM_MULTI;

    const char *data = NULL;
    size_t len = 0;
    if (hl_build_get_template(&data, &len) == 0)
        return PLATFORM_SINGLE;

    return PLATFORM_NONE;
}

#ifdef HL_ENABLE_TCC
static int tcc_is_embedded(void)
{
    const char *v = hl_build_tcc_version_string();
    return v && strcmp(v, "tcc-not-embedded") != 0;
}
#endif

/* ── Compute (WASM/AOT) capability detection ───────────────────────── */
/*
 * Reports what this hull binary can do with compute WASM modules
 * and how AOT-ready the host environment is.
 *
 * We answer four questions:
 *   1. Was the binary built with HL_ENABLE_WASM?  (yes / no — compile-time)
 *   2. Was Memory64 support compiled in?           (always yes when WASM is on
 *                                                   today, kept as a flag so
 *                                                   future builds can drop it)
 *   3. Is `wamrc` available to AOT-compile sources on this host?
 *      Looked up in PATH and as a sibling to the running hull binary.
 *   4. Is `clang` with wasm32 targeting available? (so `hull compute build`
 *      can compile `.c` sources). We piggy-back on the existing compiler
 *      discovery: any `clang` in PATH suffices on Linux (assumes lld);
 *      Homebrew llvm@18 sits in a non-PATH directory we also probe.
 */

#define HL_AOT_ARCH_THIS \
    "host"

typedef struct {
    int   wasm_enabled;          /* HL_ENABLE_WASM */
    char  wamrc_path[PATH_MAX];  /* empty = not found */
    int   wamrc_managed;         /* 1 if wamrc came from ~/.hull/tools/ */
    char  clang_path[PATH_MAX];  /* empty = not found */
    int   has_brew_llvm;         /* Homebrew llvm clang exists under /opt/homebrew or /usr/local */
    int   has_wasm_ld;           /* wasm-ld in PATH (Linux) */
    int   gpu_enabled;           /* HL_ENABLE_GPU */
} ComputeInfo;

/* Look for wamrc next to the running hull binary as well as in PATH.
 * Many devs run `make wamrc` which produces build/wamrc next to build/hull. */
static int find_sibling_executable(const char *self_path, const char *name,
                                   char *out, size_t out_sz)
{
    if (!self_path || !name || !out || out_sz == 0) return 0;
    /* Strip the basename from self_path. */
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", self_path);
    char *slash = strrchr(dir, '/');
    if (!slash) return 0;
    *slash = '\0';
    char candidate[PATH_MAX];
    int n = snprintf(candidate, sizeof(candidate), "%s/%s", dir, name);
    if (n <= 0 || (size_t)n >= sizeof(candidate)) return 0;
    if (access(candidate, X_OK) == 0) {
        snprintf(out, out_sz, "%s", candidate);
        return 1;
    }
    return 0;
}

static void detect_compute(ComputeInfo *info, const char *self_path)
{
    memset(info, 0, sizeof(*info));

#ifdef HL_ENABLE_WASM
    info->wasm_enabled = 1;
#else
    info->wasm_enabled = 0;
#endif

#ifdef HL_ENABLE_GPU
    info->gpu_enabled = 1;
#else
    info->gpu_enabled = 0;
#endif

    /* wamrc: consult the shared `hl_tools_lookup_path` so we follow the
     * same order as the build system (~/.hull/tools first, then
     * dirname(hull), then PATH). Fall back to the historical
     * sibling-to-hull / PATH probes if the canonical helper returns
     * nothing, so `./build/wamrc` is still discoverable in source-tree
     * development. */
    if (hl_tools_lookup_path("wamrc", self_path,
                             info->wamrc_path, sizeof(info->wamrc_path)) != 0) {
        if (!find_sibling_executable(self_path, "wamrc",
                                     info->wamrc_path,
                                     sizeof(info->wamrc_path)))
            find_in_path("wamrc", info->wamrc_path, sizeof(info->wamrc_path));
    }
    /* Flag whether the discovered binary is under ~/.hull/tools so the
     * renderer can produce the right hint. */
    if (info->wamrc_path[0]) {
        const char *home = getenv("HOME");
        if (home && *home) {
            char prefix[PATH_MAX];
            int pn = snprintf(prefix, sizeof(prefix), "%s/.hull/tools/", home);
            if (pn > 0 && (size_t)pn < sizeof(prefix) &&
                strncmp(info->wamrc_path, prefix, (size_t)pn) == 0) {
                info->wamrc_managed = 1;
            }
        }
    }

    /* Probe for Homebrew llvm (needed on macOS for wasm-ld). */
    const char *brew[] = {
        "/opt/homebrew/opt/llvm@18/bin/clang",
        "/opt/homebrew/opt/llvm/bin/clang",
        "/usr/local/opt/llvm@18/bin/clang",
        "/usr/local/opt/llvm/bin/clang",
        NULL,
    };
    for (const char **p = brew; *p; p++) {
        if (access(*p, X_OK) == 0) {
            snprintf(info->clang_path, sizeof(info->clang_path), "%s", *p);
            info->has_brew_llvm = 1;
            break;
        }
    }
    if (!info->clang_path[0])
        find_in_path("clang", info->clang_path, sizeof(info->clang_path));

    char wasm_ld_path[PATH_MAX];
    info->has_wasm_ld = find_in_path("wasm-ld", wasm_ld_path, sizeof(wasm_ld_path));
}

/* ── Cache info helpers (shared by human + JSON renderers) ─────── */

/* Open the blob store for `kind` and read its count + total size.
 * NULL store (path unavailable, mkdir failed, etc.) → reports
 * (0, 0). Mirrors the helper in commands/cache.c but kept local
 * so doctor doesn't depend on the cache command. */
static void cache_stats(const HlCacheKind *kind,
                        char *path_out, size_t path_out_sz,
                        uint64_t *out_count, uint64_t *out_size)
{
    *out_count = 0;
    *out_size  = 0;
    path_out[0] = '\0';

    if (hl_cache_resolve_path(kind, path_out, path_out_sz) != 0) return;

    HlBlobStore *s = NULL;
    if (hl_blob_store_open(&s, NULL, path_out, /*shard_depth=*/1, 0) != 0)
        return;
    *out_count = hl_blob_store_count(s);
    *out_size  = hl_blob_store_total_size(s);
    hl_blob_store_close(s);
}

/* Compact size formatter — mirrors commands/cache.c::format_size so
 * doctor and `hull cache list` report identically. */
static void doctor_format_size(uint64_t bytes, char *out, size_t out_sz)
{
    if (bytes >= (1ULL << 30))
        snprintf(out, out_sz, "%.1f GB", bytes / (double)(1ULL << 30));
    else if (bytes >= (1ULL << 20))
        snprintf(out, out_sz, "%.1f MB", bytes / (double)(1ULL << 20));
    else if (bytes >= (1ULL << 10))
        snprintf(out, out_sz, "%.1f KB", bytes / (double)(1ULL << 10));
    else
        snprintf(out, out_sz, "%llu B", (unsigned long long)bytes);
}

/* ── Human-readable output ──────────────────────────────────────── */

static void print_check(FILE *f, const char *label, int ok, const char *detail)
{
    if (ok)
        fprintf(f, "  %-12s \xe2\x9c\x93  %s\n", label, detail ? detail : "");
    else
        fprintf(f, "  %-12s \xe2\x9c\x97  %s\n", label, detail ? detail : "not found");
}

static void print_human(FILE *f, CompilerInfo *ci, int nci,
                        PlatformEmbed embed, int any_compiler,
                        const ComputeInfo *cmp)
{
    /* ── Binary info ── */
    fprintf(f, "hull %s  %s  %s  %s\n\n",
            HL_VERSION, doctor_runtime(), doctor_platform(), doctor_build());

    /* ── Platform library ── */
    fprintf(f, "Platform library\n");
    switch (embed) {
    case PLATFORM_MULTI:
        fprintf(f, "  embedded      multi-arch (x86_64 + aarch64)\n");
        break;
    case PLATFORM_SINGLE:
        fprintf(f, "  embedded      yes (single-arch)\n");
        break;
    case PLATFORM_NONE:
        fprintf(f, "  embedded      no\n");
        fprintf(f, "  hint          rebuild hull with EMBED_PLATFORM=1\n");
        break;
    }
    fprintf(f, "\n");

    /* ── Compilers ── */
    fprintf(f, "Compilers  (required for hull build)\n");
    for (int i = 0; i < nci; i++) {
        int found = ci[i].path[0] != '\0';
        print_check(f, ci[i].name, found, found ? ci[i].path : NULL);
    }
    fprintf(f, "\n");

#ifdef HL_ENABLE_TCC
    fprintf(f, "Embedded tcc  (compile step)\n");
    if (tcc_is_embedded()) {
        fprintf(f, "  tcc         \xe2\x9c\x93  embedded (%s)\n",
                hl_build_tcc_version_string());
    } else {
        fprintf(f, "  tcc         \xe2\x9c\x97  not embedded\n");
    }
    fprintf(f, "\n");
#endif

    /* ── CA bundle (HTTPS trust store) ── */
    {
        const unsigned char *cab_data = NULL;
        size_t cab_len = 0;
        int cab_embedded = (hl_embedded_ca_bundle(&cab_data, &cab_len) == 0);
        const char *system_paths[] = {
            "/etc/ssl/cert.pem",
            "/etc/ssl/certs/ca-certificates.crt",
            "/etc/pki/tls/certs/ca-bundle.crt",
            NULL,
        };
        const char *system_found = NULL;
        for (const char **p = system_paths; *p; p++) {
            if (access(*p, R_OK) == 0) { system_found = *p; break; }
        }

        fprintf(f, "CA bundle  (HTTPS trust store)\n");
        if (system_found) {
            fprintf(f, "  system      \xe2\x9c\x93  %s\n", system_found);
        } else {
            fprintf(f, "  system      \xe2\x9c\x97  not found at standard paths\n");
        }
        if (cab_embedded) {
            fprintf(f, "  embedded    \xe2\x9c\x93  %s, %zu bytes\n",
                    hl_embedded_ca_bundle_label(), cab_len);
        } else {
            fprintf(f, "  embedded    \xe2\x9c\x97  not embedded (rebuild with HL_EMBED_CA_BUNDLE=1)\n");
        }
        fprintf(f, "\n");
    }

    /* ── Compute (WASM) ── */
    fprintf(f, "Compute (WASM)  (compute/<name>.wasm modules)\n");
    if (!cmp->wasm_enabled) {
        fprintf(f, "  runtime     \xe2\x9c\x97  HL_ENABLE_WASM=0 — compute.* unavailable\n");
    } else {
        fprintf(f, "  runtime     \xe2\x9c\x93  WAMR enabled (interpreter + AOT loader linked in)\n");

        /* AOT precompiler (host toolchain). */
        if (cmp->wamrc_path[0]) {
            fprintf(f, "  wamrc       \xe2\x9c\x93  %s%s\n",
                    cmp->wamrc_path,
                    cmp->wamrc_managed ? "  (managed via `hull tools`)" : "");
            fprintf(f, "                (hull build will auto-AOT-compile compute/*.wasm)\n");
        } else {
            fprintf(f, "  wamrc       \xe2\x97\x8b  not installed\n");
            fprintf(f, "                hint: `hull tools install wamrc` "
                       "(signed download)\n");
            fprintf(f, "                       or `make wamrc` (build from source)\n");
            fprintf(f, "                without wamrc, modules run via the fast interpreter\n");
            fprintf(f, "                (~50x slower than AOT for compute-heavy work)\n");
        }

        /* Source toolchain (for `hull compute build`). */
        if (cmp->clang_path[0]) {
            fprintf(f, "  clang       \xe2\x9c\x93  %s\n", cmp->clang_path);
            if (cmp->has_brew_llvm) {
                fprintf(f, "                (Homebrew llvm — bundles wasm-ld)\n");
            } else if (cmp->has_wasm_ld) {
                fprintf(f, "                (with wasm-ld in PATH)\n");
            } else {
                fprintf(f, "                \xe2\x9a\xa0  wasm-ld not found — install lld\n");
            }
        } else {
            fprintf(f, "  clang       \xe2\x9c\x97  not found — `hull compute build` cannot compile sources\n");
            fprintf(f, "                hint: brew install llvm@18 (macOS) / apt install clang lld (Linux)\n");
        }
    }
    fprintf(f, "\n");

    /* ── Compute (GPU) ── */
    fprintf(f, "Compute (GPU)   (shaders/<name>.wgsl)\n");
    if (cmp->gpu_enabled) {
        fprintf(f, "  runtime     \xe2\x9c\x93  wgpu-native linked in\n");
    } else {
        fprintf(f, "  runtime     \xe2\x97\x8b  not built (rebuild with `make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu`)\n");
    }
    fprintf(f, "\n");

    /* ── Module subsystems ── */
    /* Summarises which capability bits the *build* satisfies, then walks
     * the registry to flag any modules that can never be admitted in
     * this binary (e.g. `hull/gpu` when HL_ENABLE_GPU=0). The full
     * resolver runs at app startup; this is purely a build-side hint. */
    {
        fprintf(f, "Module subsystems  (build-time capabilities)\n");
#ifdef HL_ENABLE_DB
        fprintf(f, "  HL_ENABLE_DB    \xe2\x9c\x93  hull/db, hull/middleware/{session,csrf,auth,...} importable\n");
#else
        fprintf(f, "  HL_ENABLE_DB    \xe2\x97\x8b  off — hull/db and DB-dependent middleware will fail to resolve\n");
#endif
#ifdef HL_ENABLE_WASM
        fprintf(f, "  HL_ENABLE_WASM  \xe2\x9c\x93  hull/compute importable\n");
#else
        fprintf(f, "  HL_ENABLE_WASM  \xe2\x97\x8b  off — hull/compute will fail to resolve\n");
#endif
#ifdef HL_ENABLE_GPU
        fprintf(f, "  HL_ENABLE_GPU   \xe2\x9c\x93  hull/gpu importable\n");
#else
        fprintf(f, "  HL_ENABLE_GPU   \xe2\x97\x8b  off — hull/gpu will fail to resolve\n");
#endif
#ifdef HL_ENABLE_HTTP
        fprintf(f, "  HL_ENABLE_HTTP  \xe2\x9c\x93  hull/http-server, hull/http-client, hull/web/* (ws/sse/middleware) importable\n");
#else
        fprintf(f, "  HL_ENABLE_HTTP  \xe2\x97\x8b  off — CLI / compute-only build; hull/http-*, hull/web/ws-*, hull/web/sse, and hull/web/middleware/* will fail to resolve\n");
#endif
        size_t total = 0;
        (void)hl_module_registry_all(&total);
        fprintf(f, "  registry        %zu first-party modules — run `hull modules available` for the full list\n",
                total);
        fprintf(f, "\n");
    }

    /* ── Caches ── */
    /* Walks the same registry that powers `hull cache list` so the
     * two surfaces are always in lockstep. Status indicators:
     *   ✓  store exists with entries on disk
     *   ○  no entries yet (cache cold)
     *   ✗  path unresolvable (e.g. HOME unset)
     *
     * The footer line surfaces an active HULL_CACHE_DIR override so
     * users running multi-tenant deployments can see at a glance
     * which path their caches are landing in.
     *
     * Size-bloat hints: caches are unbounded by design (correctness
     * doesn't depend on freshness — stale entries are harmless
     * orphans). But once any single kind passes 250 MB or the
     * runtime total passes 1 GB, surface a `⚠` next to the size
     * and suggest a prune. These thresholds are deliberately
     * conservative — most dev workstations should never see them. */
    #define HL_DOCTOR_CACHE_KIND_WARN_BYTES   (250ULL * 1024 * 1024)
    #define HL_DOCTOR_CACHE_TOTAL_WARN_BYTES  (1024ULL * 1024 * 1024)
    {
        fprintf(f, "Caches  (runtime + tools storage)\n");
        uint64_t runtime_count = 0, runtime_bytes = 0;
        int large_kinds = 0;
        for (const HlCacheKind *k = hl_cache_registry(); k->name; k++) {
            char path[PATH_MAX];
            uint64_t cnt = 0, size = 0;
            cache_stats(k, path, sizeof(path), &cnt, &size);

            const char *mark;
            if (path[0] == '\0')   mark = "\xe2\x9c\x97";  /* ✗ */
            else if (cnt == 0)     mark = "\xe2\x97\x8b";  /* ○ */
            else                   mark = "\xe2\x9c\x93";  /* ✓ */

            char size_str[32];
            doctor_format_size(size, size_str, sizeof(size_str));
            const char *warn = "";
            if (size >= HL_DOCTOR_CACHE_KIND_WARN_BYTES) {
                warn = "  \xe2\x9a\xa0 large";  /* ⚠ large */
                large_kinds++;
            }
            fprintf(f, "  %-12s %s  %llu entries, %s%s\n",
                    k->name, mark,
                    (unsigned long long)cnt, size_str, warn);
            if (path[0] != '\0')
                fprintf(f, "                %s%s\n", path,
                        k->is_runtime ? "" : "  (system store)");
            if (k->is_runtime) {
                runtime_count += cnt;
                runtime_bytes += size;
            }
        }
        char total_str[32];
        doctor_format_size(runtime_bytes, total_str, sizeof(total_str));
        const char *total_warn =
            (runtime_bytes >= HL_DOCTOR_CACHE_TOTAL_WARN_BYTES)
                ? "  \xe2\x9a\xa0 large" : "";
        fprintf(f, "                runtime total: %llu entries, %s%s\n",
                (unsigned long long)runtime_count, total_str,
                total_warn);

        const char *override = getenv("HULL_CACHE_DIR");
        if (override && *override) {
            fprintf(f, "                HULL_CACHE_DIR active: %s\n",
                    override);
        }
        fprintf(f, "                manage via `hull cache list|prune|clear`\n");

        /* If anything tripped the warning, surface a concrete hint
         * — bare "manage via" doesn't tell users what to actually
         * type. */
        if (large_kinds > 0 ||
            runtime_bytes >= HL_DOCTOR_CACHE_TOTAL_WARN_BYTES) {
            fprintf(f,
                "                hint: `hull cache prune "
                "--max-age=30d --strategy=lru`\n"
                "                       reclaims entries not "
                "touched in the last 30 days.\n");
        }
        fprintf(f, "\n");
    }

    /* ── Summary ── */
    fprintf(f, "hull build    ");
    if (embed == PLATFORM_NONE) {
        fprintf(f, "not ready — platform library not embedded\n");
        fprintf(f, "              hint: make platform && make EMBED_PLATFORM=1\n");
    } else {
#ifdef HL_ENABLE_TCC
        int tcc_avail = tcc_is_embedded();
#else
        int tcc_avail = 0;
#endif
        if (!any_compiler && !tcc_avail) {
            fprintf(f, "not ready — no C compiler found\n");
            fprintf(f, "              hint: install gcc or clang\n");
        } else if (!any_compiler) {
            fprintf(f, "ready (embedded tcc for compile, system linker for link)\n");
        } else {
            fprintf(f, "ready\n");
        }
    }
}

/* ── JSON output ────────────────────────────────────────────────── */

/* Generic FILE* writer for ShJsonWriter (same shape as in
 * commands/cache.c and sbom.c). */
static int stdio_write_fn(void *ctx, const char *data, size_t len)
{
    FILE *fp = (FILE *)ctx;
    return fwrite(data, 1, len, fp) == len ? 0 : -1;
}

/* Helper: write a string value, or null if the C string is empty. */
static void emit_string_or_null(ShJsonWriter *w, const char *key,
                                const char *val)
{
    if (val && *val) sh_json_write_kv_string(w, key, val);
    else             sh_json_write_kv_null(w, key);
}

static void print_json(FILE *f, CompilerInfo *ci, int nci,
                       PlatformEmbed embed, int any_compiler,
                       const ComputeInfo *cmp)
{
    const char *embed_str =
        embed == PLATFORM_MULTI  ? "multi-arch" :
        embed == PLATFORM_SINGLE ? "single-arch" : "none";

#ifdef HL_ENABLE_TCC
    int tcc_avail_json = tcc_is_embedded();
#else
    int tcc_avail_json = 0;
#endif
    const char *ready_str =
        (embed == PLATFORM_NONE)              ? "no-platform" :
        (!any_compiler && !tcc_avail_json)    ? "no-compiler"  : "ready";

    ShJsonWriter w;
    sh_json_writer_init(&w, stdio_write_fn, f);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "version",            HL_VERSION);
    sh_json_write_kv_string(&w, "runtime",            doctor_runtime());
    sh_json_write_kv_string(&w, "platform",           doctor_platform());
    sh_json_write_kv_string(&w, "build",              doctor_build());
    sh_json_write_kv_string(&w, "platform_embedded",  embed_str);

    sh_json_write_key(&w, "compilers");
    sh_json_write_array_start(&w);
    for (int i = 0; i < nci; i++) {
        sh_json_write_object_start(&w);
        sh_json_write_kv_string(&w, "name", ci[i].name);
        emit_string_or_null(&w, "path", ci[i].path);
        sh_json_write_object_end(&w);
    }
    sh_json_write_array_end(&w);

#ifdef HL_ENABLE_TCC
    sh_json_write_kv_bool(&w, "tcc_embedded", tcc_is_embedded() != 0);
#endif

    /* CA bundle status */
    {
        const unsigned char *cab_data = NULL;
        size_t cab_len = 0;
        int cab_embedded = (hl_embedded_ca_bundle(&cab_data, &cab_len) == 0);
        const char *system_paths[] = {
            "/etc/ssl/cert.pem",
            "/etc/ssl/certs/ca-certificates.crt",
            "/etc/pki/tls/certs/ca-bundle.crt",
            NULL,
        };
        const char *system_found = NULL;
        for (const char **p = system_paths; *p; p++) {
            if (access(*p, R_OK) == 0) { system_found = *p; break; }
        }
        sh_json_write_key(&w, "ca_bundle");
        sh_json_write_object_start(&w);
        emit_string_or_null(&w, "system", system_found);
        sh_json_write_kv_bool(&w, "embedded", cab_embedded != 0);
        if (cab_embedded)
            sh_json_write_kv_string(&w, "embedded_label",
                                    hl_embedded_ca_bundle_label());
        sh_json_write_object_end(&w);
    }

    /* Compute capability surface. */
    {
        int aot_ready = cmp->wasm_enabled && cmp->wamrc_path[0] != '\0';
        sh_json_write_key(&w, "compute");
        sh_json_write_object_start(&w);
        sh_json_write_kv_bool  (&w, "wasm_enabled",   cmp->wasm_enabled != 0);
        sh_json_write_kv_bool  (&w, "gpu_enabled",    cmp->gpu_enabled  != 0);
        emit_string_or_null    (&w, "wamrc",          cmp->wamrc_path);
        sh_json_write_kv_bool  (&w, "wamrc_managed",  cmp->wamrc_managed != 0);
        emit_string_or_null    (&w, "clang",          cmp->clang_path);
        sh_json_write_kv_bool  (&w, "wasm_ld",        cmp->has_wasm_ld != 0);
        sh_json_write_kv_bool  (&w, "aot_ready",      aot_ready != 0);
        sh_json_write_object_end(&w);
    }

    /* Module-subsystem capability bits — mirrors build_provided_caps()
     * in module_resolver.c. */
    sh_json_write_key(&w, "subsystems");
    sh_json_write_object_start(&w);
#ifdef HL_ENABLE_DB
    sh_json_write_kv_bool(&w, "db", true);
#else
    sh_json_write_kv_bool(&w, "db", false);
#endif
#ifdef HL_ENABLE_WASM
    sh_json_write_kv_bool(&w, "wasm", true);
#else
    sh_json_write_kv_bool(&w, "wasm", false);
#endif
#ifdef HL_ENABLE_GPU
    sh_json_write_kv_bool(&w, "gpu", true);
#else
    sh_json_write_kv_bool(&w, "gpu", false);
#endif
#ifdef HL_ENABLE_HTTP
    sh_json_write_kv_bool(&w, "http", true);
#else
    sh_json_write_kv_bool(&w, "http", false);
#endif
    sh_json_write_object_end(&w);

    /* Cache status — same registry as `hull cache list`. */
    sh_json_write_key(&w, "caches");
    sh_json_write_array_start(&w);
    for (const HlCacheKind *k = hl_cache_registry(); k->name; k++) {
        char path[PATH_MAX];
        uint64_t cnt = 0, size = 0;
        cache_stats(k, path, sizeof(path), &cnt, &size);

        sh_json_write_object_start(&w);
        sh_json_write_kv_string(&w, "name",       k->name);
        sh_json_write_kv_bool  (&w, "is_runtime", k->is_runtime != 0);
        emit_string_or_null    (&w, "path",       path);
        sh_json_write_kv_int   (&w, "count",      (int64_t)cnt);
        sh_json_write_kv_int   (&w, "size_bytes", (int64_t)size);
        sh_json_write_object_end(&w);
    }
    sh_json_write_array_end(&w);

    {
        const char *override = getenv("HULL_CACHE_DIR");
        emit_string_or_null(&w, "hull_cache_dir", override);
    }
    sh_json_write_kv_string(&w, "hull_build", ready_str);
    sh_json_write_object_end(&w);
    fputc('\n', f);
}

/* ── Public collector ────────────────────────────────────────────── */

void hl_doctor_collect_json(FILE *f)
{
    if (!f) return;
    CompilerInfo ci[MAX_COMPILERS] = {
        { "cc",      {0} },
        { "gcc",     {0} },
        { "clang",   {0} },
        { "cosmocc", {0} },
    };
    discover_compilers(ci, MAX_COMPILERS);

    int any_compiler = 0;
    for (int i = 0; i < MAX_COMPILERS; i++) {
        if (ci[i].path[0]) { any_compiler = 1; break; }
    }

    PlatformEmbed embed = detect_platform();

    ComputeInfo cmp;
    /* hull_exe lookup not available here — pass NULL; only wamrc/clang
     * paths near the binary are skipped, PATH lookup still works. */
    detect_compute(&cmp, NULL);

    print_json(f, ci, MAX_COMPILERS, embed, any_compiler, &cmp);
}

/* ── Handler ─────────────────────────────────────────────────────── */

int hl_cmd_doctor(int argc, char **argv, const HlCommandEnv *env)
{
    int json = 0;
    int tui  = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0)
            json = 1;
        else if (strcmp(argv[i], "--tui") == 0)
            tui = 1;
    }

#ifdef HL_ENABLE_TUI
    if (tui) {
        /* Refuse cleanly when not attached to a real terminal — the
         * cap layer would otherwise error out mid-acquire and leave a
         * cryptic message. Surface the situation here. */
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
            fprintf(stderr,
                "hull doctor --tui: not attached to a terminal "
                "(stdin/stdout redirected). Try running directly in a "
                "terminal, or use `hull doctor` / `hull doctor --json`.\n");
            return 1;
        }
        return hull_tool("hull.doctor_tui", argc, argv, env->hull_exe);
    }
#else
    if (tui) {
        fprintf(stderr,
            "hull doctor --tui: this build was compiled without "
            "HL_ENABLE_TUI; use plain `hull doctor`.\n");
        return 1;
    }
#endif

    CompilerInfo ci[MAX_COMPILERS] = {
        { "cc",      {0} },
        { "gcc",     {0} },
        { "clang",   {0} },
        { "cosmocc", {0} },
    };
    discover_compilers(ci, MAX_COMPILERS);

    int any_compiler = 0;
    for (int i = 0; i < MAX_COMPILERS; i++) {
        if (ci[i].path[0]) { any_compiler = 1; break; }
    }

    PlatformEmbed embed = detect_platform();

    ComputeInfo cmp;
    detect_compute(&cmp, env->hull_exe);

    if (json)
        print_json(stdout, ci, MAX_COMPILERS, embed, any_compiler, &cmp);
    else
        print_human(stdout, ci, MAX_COMPILERS, embed, any_compiler, &cmp);

    /* Exit 1 if hull build cannot work, so scripts can check: hull doctor || ... */
#ifdef HL_ENABLE_TCC
    int tcc_available = tcc_is_embedded();
#else
    int tcc_available = 0;
#endif
    /* hull build is ready when platform is embedded AND a compiler exists */
    int build_ready = (embed != PLATFORM_NONE) && (any_compiler || tcc_available);
    return build_ready ? 0 : 1;
}
