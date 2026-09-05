/*
 * commands/doctor.c - hull doctor subcommand
 *
 * Checks the local environment and reports whether hull is ready to
 * build, develop, and deploy applications.
 *
 *   hull doctor          - human-readable output
 *   hull doctor --json   - machine-readable JSON
 *
 * Checks performed:
 *   1. Hull binary metadata (version, runtime, platform, build mode)
 *   2. Platform library embedding (none / single-arch / multi-arch)
 *   3. C compiler availability in PATH (cc, gcc, clang, cosmocc)
 *   4. WASM compute capability (HL_ENABLE_WASM, AOT loader, wamrc)
 *   5. GPU compute capability (HL_ENABLE_GPU)
 *   6. Overall hull build readiness summary
 *
 * Doctor is the ONBOARDING surface, so every negative result must say what to
 * DO, in a form that works on the host the user is actually on, and must
 * distinguish four states instead of collapsing them into one red cross:
 *
 *   OK    v  present / working
 *   MISS  x  REQUIRED and absent - blocks something, and has a fix
 *   OPT   o  optional and absent - nothing is broken
 *   FALL  ~  a system facility is absent but a working FALLBACK is active
 *
 * The rule that motivated this: a cosmo hull (the build that reaches Windows)
 * cannot use cc/gcc/clang - it embeds cosmo-format platform archives, so only
 * cosmocc links a working APE (see hl_driver_resolve_native in compiler.c).
 * Telling a Windows user to "install gcc or clang" neither fixes the problem
 * nor points at Hull's own `hull tools install cosmocc`, which ships exactly
 * the right toolchain.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/doctor.h"
#include "hull/shared/blob_store.h"
#include "hull/build_assets.h"
#include "hull/shared/cache_registry.h"
#include "hull/cacert.h"
#include "hull/compiler.h"
#include "hull/module_registry.h"
#include "hull/shared/host.h"
#include "hull/tool.h"
#include "hull/tools_install.h"
#ifdef HL_ENABLE_HTTP_CLIENT
#include "hull/commands/tools.h"
#endif
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

/* Walk PATH and return the first directory containing `name`.
 *
 * Delegates to the shared host resolver (src/hull/shared/host.c). The previous
 * local implementation split PATH on ':' and joined with '/', which cannot
 * work on Windows: PATH is ';'-separated and its components are `C:\...`, so
 * the ':' split shredded every drive letter and EVERY compiler probe reported
 * "not found" no matter what was installed. hl_host_find_in_path splits on the
 * host separator and also tries the .exe/.com forms there. */
static int find_in_path(const char *name, char *out, size_t out_sz)
{
    return hl_host_find_in_path(name, out, out_sz);
}

static void discover_compilers(CompilerInfo *ci, int count, const char *hull_exe)
{
    for (int i = 0; i < count; i++)
        ci[i].path[0] = '\0';

    for (int i = 0; i < count; i++) {
        find_in_path(ci[i].name, ci[i].path, sizeof(ci[i].path));
        /* cosmocc is a hull-managed TOOL, not typically on PATH: `hull tools
         * install cosmocc` extracts it to ~/.hull/tools/cosmocc/bin/cosmocc,
         * which is what `hull build` resolves via the compiler driver. Consult
         * the same shared resolver so doctor agrees with build (a PATH-only
         * probe would report a hull-installed cosmocc as missing). */
        if (!ci[i].path[0] && strcmp(ci[i].name, "cosmocc") == 0) {
            HlToolStatus cs;
            if (hl_tools_status("cosmocc", hull_exe, &cs) == 0 && cs.resolved)
                snprintf(ci[i].path, sizeof(ci[i].path), "%s", cs.path);
        }
    }
}

/* ── Which compiler actually satisfies `hull build`? ────────────────
 *
 * Not "any of the four". A COSMO hull embeds cosmo-format platform archives;
 * cc/gcc/clang would link those to ELF/Mach-O rather than a portable APE, so
 * cosmocc is the only driver that yields a working artifact. A NATIVE hull is
 * the mirror image: cc/gcc/clang are the real drivers and cosmocc is never
 * consulted. This mirrors the preference order in hl_driver_resolve_native()
 * (src/hull/compiler.c) so doctor's verdict and build's behaviour agree.
 *
 * Before this distinction existed, a cosmo hull with a stray gcc on PATH
 * reported `hull build  ready` and then failed at the link step.
 *
 * Returns an index into `ci`, or -1 when nothing usable is present. */
static int doctor_build_compiler(const CompilerInfo *ci, int n)
{
#ifdef __COSMOPOLITAN__
    for (int i = 0; i < n; i++)
        if (strcmp(ci[i].name, "cosmocc") == 0 && ci[i].path[0])
            return i;
    return -1;
#else
    for (int i = 0; i < n; i++) {
        if (strcmp(ci[i].name, "cosmocc") == 0) continue;
        if (ci[i].path[0]) return i;
    }
    return -1;
#endif
}

/* Is `cosmocc` the compiler this binary needs? True exactly on a cosmo hull -
 * the build that reaches Windows. */
static int doctor_wants_cosmocc(void)
{
#ifdef __COSMOPOLITAN__
    return 1;
#else
    return 0;
#endif
}

/* The single actionable command that makes `hull build` work here, or NULL
 * when no Hull-managed fix exists and a system toolchain is required. */
static const char *doctor_compiler_fix(void)
{
#if defined(__COSMOPOLITAN__) && defined(HL_ENABLE_HTTP_CLIENT)
    return "hull tools install cosmocc";
#else
    return NULL;
#endif
}

/* ── HTTPS trust store ──────────────────────────────────────────────
 *
 * The system store is a CONVENIENCE, not a requirement: Hull embeds Mozilla's
 * CA bundle precisely so HTTPS works on hosts without one (Windows, scratch
 * containers, air-gapped images). Rendering its absence as a red cross made a
 * perfectly healthy Windows install look broken, so it is reported as "using
 * the fallback"; only the case where NEITHER exists is a failure.
 *
 * Returns the system bundle path, or NULL when none is present. Shared by the
 * human and JSON renderers so they can never disagree. */
static const char *doctor_system_ca(void)
{
    static const char *system_paths[] = {
        "/etc/ssl/cert.pem",
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        NULL,
    };
    for (const char **q = system_paths; *q; q++)
        if (access(*q, R_OK) == 0) return *q;
    return NULL;
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

/* ── Compute (WASM/AOT) capability detection ───────────────────────── */
/*
 * Reports what this hull binary can do with compute WASM modules
 * and how AOT-ready the host environment is.
 *
 * We answer four questions:
 *   1. Was the binary built with HL_ENABLE_WASM?  (yes / no - compile-time)
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

    /* wamrc: the shared `hl_tools_status` (built on `hl_tools_lookup_path`) is
     * the SINGLE resolver `hull tools list` / `hull agent tools` / `hull build`
     * also use - same order (~/.hull/tools → beside hull → PATH), same
     * managed-install classification. The `dirname(hull)/wamrc` step covers the
     * source-tree `./build/wamrc` (it sits beside `./build/hull`); the PATH step
     * covers a system install. */
    {
        HlToolStatus ws;
        hl_tools_status("wamrc", self_path, &ws);
        if (ws.resolved) {
            snprintf(info->wamrc_path, sizeof(info->wamrc_path), "%s", ws.path);
            info->wamrc_managed = ws.managed;
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

/* Compact size formatter - mirrors commands/cache.c::format_size so
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

/* Status glyphs. Doctor distinguishes four states, because "missing",
 * "not applicable" and "falling back" mean very different things to a user
 * deciding whether their install is broken. See the header comment. */
#define GLYPH_OK   "\xe2\x9c\x93"  /* checkmark */
#define GLYPH_MISS "\xe2\x9c\x97"  /* ballot x  */
#define GLYPH_OPT  "\xe2\x97\x8b"  /* circle    */
#define GLYPH_FALL "\xe2\x86\xb3"  /* arrow     */

static void print_row(FILE *f, const char *label, const char *glyph,
                      const char *detail)
{
    fprintf(f, "  %-12s %s  %s\n", label, glyph, detail ? detail : "");
}

/* The at-a-glance verdict, printed FIRST so "is my install OK?" does not
 * require reading six detail sections. `ca` is 2 = system store, 1 = embedded
 * bundle only, 0 = neither. */
static void print_summary(FILE *f, PlatformEmbed embed, int build_cc,
                          const CompilerInfo *ci, int ca)
{
    /* "none" is reachable: RUNTIME=lua / RUNTIME=js drop the other
     * interpreter, and a build with neither cannot load an app at all. Do not
     * claim ready in that case. */
    if (strcmp(doctor_runtime(), "none") == 0)
        fprintf(f, "Runtime       none - this build has no script runtime\n");
    else
        fprintf(f, "Runtime       ready  (%s, %s)\n",
                doctor_runtime(), doctor_platform());

#ifdef HL_ENABLE_HTTP
    if (ca)
        fprintf(f, "HTTPS         ready  (%s)\n",
                ca == 2 ? "system CA store" : "embedded CA bundle");
    else
        fprintf(f, "HTTPS         no trust store\n");
#else
    (void)ca;
    fprintf(f, "HTTPS         not built in (HL_ENABLE_HTTP=0)\n");
#endif

    if (embed == PLATFORM_NONE)
        fprintf(f, "Build         platform library not embedded\n");
    else if (build_cc < 0)
        fprintf(f, "Build         compiler not installed\n");
    else
        fprintf(f, "Build         ready  (%s)\n", ci[build_cc].name);
    fprintf(f, "\n");
}

static void print_human(FILE *f, CompilerInfo *ci, int nci,
                        PlatformEmbed embed, int build_cc,
                        const ComputeInfo *cmp)
{
    /* Binary info */
    fprintf(f, "hull %s  %s  %s  %s\n\n",
            HL_VERSION, doctor_runtime(), doctor_platform(), doctor_build());

    {
        const unsigned char *cab = NULL;
        size_t cab_len = 0;
        int embedded = (hl_embedded_ca_bundle(&cab, &cab_len) == 0);
        int system_ok = (doctor_system_ca() != NULL);
        print_summary(f, embed, build_cc, ci,
                      system_ok ? 2 : (embedded ? 1 : 0));
    }

    /* Platform library.
     *
     * NOTE the two "arch" notions users conflate: THIS line is about how many
     * platform ARCHIVES this hull carries to link apps against. `hull build`
     * separately reports a "dual-arch" APE LINK, a property of the cosmocc
     * link step. The wording says "app link targets" so the two do not read as
     * contradicting each other. */
    fprintf(f, "Platform library  (archives this hull links apps against)\n");
    switch (embed) {
    case PLATFORM_MULTI:
        print_row(f, "embedded", GLYPH_OK,
                  "2 app link targets: x86_64 + aarch64 "
                  "(a cosmo/APE app links BOTH)");
        break;
    case PLATFORM_SINGLE:
        print_row(f, "embedded", GLYPH_OK, "1 app link target (this arch)");
        break;
    case PLATFORM_NONE:
        print_row(f, "embedded", GLYPH_MISS, "none - hull build cannot link");
        fprintf(f, "                source build: make platform && "
                   "make EMBED_PLATFORM=1\n");
        break;
    }
    fprintf(f, "\n");

    /* Compilers.
     *
     * Only ONE of these can drive `hull build` for this binary (see
     * doctor_build_compiler): cosmocc on a cosmo hull, cc/gcc/clang on a
     * native one. The others are marked informational rather than failing, so
     * the report does not show three red crosses for tools this build would
     * never have used anyway. */
    {
        int want_cosmocc = doctor_wants_cosmocc();
        fprintf(f, "Compilers  (hull build needs %s)\n",
                want_cosmocc ? "cosmocc" : "one of cc / gcc / clang");
        for (int i = 0; i < nci; i++) {
            int found    = ci[i].path[0] != '\0';
            int is_cos   = (strcmp(ci[i].name, "cosmocc") == 0);
            int relevant = want_cosmocc ? is_cos : !is_cos;
            if (found)
                print_row(f, ci[i].name, GLYPH_OK, ci[i].path);
            else if (relevant)
                print_row(f, ci[i].name, GLYPH_MISS, "not found");
            else
                print_row(f, ci[i].name, GLYPH_OPT,
                          "not found (not used by this hull)");
        }
        if (build_cc < 0) {
            const char *fix = doctor_compiler_fix();
            if (fix) {
                /* Hull ships the right toolchain: recommend Hull's own
                 * bootstrap rather than a package manager that may not even
                 * exist on this host. */
                fprintf(f, "                fix: %s\n", fix);
                fprintf(f, "                     (signed download, no admin "
                           "rights, nothing outside ~/.hull)\n");
                fprintf(f, "                or:  hull doctor --fix\n");
            } else if (want_cosmocc) {
                fprintf(f, "                fix: install the Cosmopolitan "
                           "toolchain (cosmocc) on PATH\n");
            } else {
                fprintf(f, "                fix: install a C toolchain (gcc "
                           "or clang) via your package manager\n");
            }
        }
        fprintf(f, "\n");
    }

    /* HTTPS trust store. An absent SYSTEM store is not a failure when the
     * embedded bundle is present - that is the designed fallback, and the
     * normal state on Windows. */
    {
        const unsigned char *cab_data = NULL;
        size_t cab_len = 0;
        int cab_embedded = (hl_embedded_ca_bundle(&cab_data, &cab_len) == 0);
        const char *system_found = doctor_system_ca();

        fprintf(f, "CA bundle  (HTTPS trust store)\n");
        if (system_found)
            print_row(f, "system", GLYPH_OK, system_found);
        else if (cab_embedded)
            print_row(f, "system", GLYPH_FALL,
                      "none on this host - using the embedded bundle "
                      "(expected on Windows)");
        else
            print_row(f, "system", GLYPH_MISS, "not found at standard paths");

        if (cab_embedded) {
            char detail[160];
            snprintf(detail, sizeof(detail), "%s, %zu bytes",
                     hl_embedded_ca_bundle_label(), cab_len);
            print_row(f, "embedded", GLYPH_OK, detail);
        } else {
            print_row(f, "embedded", GLYPH_MISS, "not embedded");
            fprintf(f, "                source build: "
                       "make HL_EMBED_CA_BUNDLE=1\n");
        }

        if (!system_found && !cab_embedded)
            fprintf(f, "                HTTPS will fail: no trust store. "
                       "Pass --ca-bundle PATH.\n");
        fprintf(f, "\n");
    }

    /* Compute (WASM). Both sub-checks are OPTIONAL capabilities: an app with
     * no compute/ directory needs neither, and an app that ships prebuilt
     * .wasm modules needs no host toolchain at all. Report them that way. */
    fprintf(f, "Compute (WASM)  (compute/<name>.wasm modules)\n");
    if (!cmp->wasm_enabled) {
        print_row(f, "runtime", GLYPH_OPT,
                  "HL_ENABLE_WASM=0 - compute.* unavailable in this build");
    } else {
        print_row(f, "runtime", GLYPH_OK,
                  "WAMR enabled (interpreter + AOT loader linked in)");

        /* AOT precompiler (host toolchain). */
        if (cmp->wamrc_path[0]) {
            char detail[PATH_MAX + 48];
            snprintf(detail, sizeof(detail), "%s%s", cmp->wamrc_path,
                     cmp->wamrc_managed ? "  (managed via `hull tools`)" : "");
            print_row(f, "wamrc", GLYPH_OK, detail);
            fprintf(f, "                hull build will auto-AOT-compile "
                       "compute/*.wasm\n");
        } else {
            print_row(f, "wamrc", GLYPH_OPT, "not installed (optional)");
            fprintf(f, "                install: `hull tools install wamrc` "
                       "(signed download)\n");
            fprintf(f, "                without it, modules run via the fast "
                       "interpreter\n");
            fprintf(f, "                (~50x slower than AOT for "
                       "compute-heavy work)\n");
            fprintf(f, "                source build: `make wamrc`\n");
        }

        /* Source toolchain - needed ONLY to compile compute/<name>/<name>.c
         * to .wasm. Hull ships no managed clang, so the hint must name a
         * route that exists on THIS host rather than printing macOS/Linux
         * package-manager commands to a Windows user. */
        if (cmp->clang_path[0]) {
            print_row(f, "clang", GLYPH_OK, cmp->clang_path);
            if (cmp->has_brew_llvm)
                fprintf(f, "                (Homebrew llvm - bundles "
                           "wasm-ld)\n");
            else if (cmp->has_wasm_ld)
                fprintf(f, "                (with wasm-ld in PATH)\n");
            else
                fprintf(f, "                %s  wasm-ld not found - install "
                           "lld to link .wasm\n", GLYPH_FALL);
        } else {
            print_row(f, "clang", GLYPH_OPT,
                      "not found (only needed to compile compute/*.c "
                      "to .wasm)");
            if (hl_host_is_windows())
                fprintf(f, "                install: `winget install "
                           "LLVM.LLVM` (clang + wasm-ld)\n");
            else
                fprintf(f, "                install: `brew install llvm@18` "
                           "(macOS) / `apt install clang lld` (Linux)\n");
            fprintf(f, "                not needed if you ship prebuilt "
                       "compute/*.wasm\n");
        }
    }
    fprintf(f, "\n");

    /* Compute (GPU). A composable FEATURE published for native platforms
     * only - so on a cosmo/APE build its absence is a fact about the build,
     * not something the user can or should fix. Never point a release user at
     * a Makefile as if it were the normal route. */
    fprintf(f, "Compute (GPU)   (shaders/<name>.wgsl)\n");
    if (cmp->gpu_enabled) {
        print_row(f, "runtime", GLYPH_OK, "wgpu-native linked in");
    } else if (strcmp(doctor_platform(), "cosmo") == 0) {
        print_row(f, "runtime", GLYPH_OPT,
                  "not available in a cosmo/APE build "
                  "(gpu is a native-only feature)");
    } else {
        print_row(f, "runtime", GLYPH_OPT, "not linked into this hull");
        fprintf(f, "                apps compose it at build time:\n");
        fprintf(f, "                  hull feature install gpu\n");
        fprintf(f, "                  hull build --with=gpu\n");
        fprintf(f, "                developer/source build (for `hull dev`): "
                   "make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu\n");
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
        fprintf(f, "  HL_ENABLE_DB    \xe2\x97\x8b  off - hull/db and DB-dependent middleware will fail to resolve\n");
#endif
#ifdef HL_ENABLE_WASM
        fprintf(f, "  HL_ENABLE_WASM  \xe2\x9c\x93  hull/compute importable\n");
#else
        fprintf(f, "  HL_ENABLE_WASM  \xe2\x97\x8b  off - hull/compute will fail to resolve\n");
#endif
#ifdef HL_ENABLE_GPU
        fprintf(f, "  HL_ENABLE_GPU   \xe2\x9c\x93  hull/gpu importable\n");
#else
        fprintf(f, "  HL_ENABLE_GPU   \xe2\x97\x8b  off - hull/gpu will fail to resolve\n");
#endif
#ifdef HL_ENABLE_HTTP
        fprintf(f, "  HL_ENABLE_HTTP  \xe2\x9c\x93  hull/http-server, hull/http-client, hull/web/* (ws/sse/middleware) importable\n");
#else
        fprintf(f, "  HL_ENABLE_HTTP  \xe2\x97\x8b  off - CLI / compute-only build; hull/http-*, hull/web/ws-*, hull/web/sse, and hull/web/middleware/* will fail to resolve\n");
#endif
        size_t total = 0;
        (void)hl_module_registry_all(&total);
        fprintf(f, "  registry        %zu first-party modules - run `hull modules available` for the full list\n",
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
     * doesn't depend on freshness - stale entries are harmless
     * orphans). But once any single kind passes 250 MB or the
     * runtime total passes 1 GB, surface a `⚠` next to the size
     * and suggest a prune. These thresholds are deliberately
     * conservative - most dev workstations should never see them. */
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
         * - bare "manage via" doesn't tell users what to actually
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

    /* Closing verdict. Mirrors print_summary's Build line but carries the
     * concrete next command, so a reader who scrolled past the top still ends
     * with something to type. */
    fprintf(f, "hull build    ");
    if (embed == PLATFORM_NONE) {
        fprintf(f, "not ready - platform library not embedded\n");
        fprintf(f, "              source build: make platform && "
                   "make EMBED_PLATFORM=1\n");
    } else if (build_cc < 0) {
        const char *fix = doctor_compiler_fix();
        fprintf(f, "not ready - no usable C compiler\n");
        if (fix) {
            fprintf(f, "\nFix:\n  %s\n", fix);
            fprintf(f, "\nOptional:\n  hull doctor --fix"
                       "   (runs the above for you)\n");
        } else if (doctor_wants_cosmocc()) {
            fprintf(f, "              install the Cosmopolitan toolchain "
                       "(cosmocc) on PATH\n");
        } else {
            fprintf(f, "              install gcc or clang\n");
        }
    } else {
        fprintf(f, "ready\n");
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
                       PlatformEmbed embed, int build_cc,
                       const ComputeInfo *cmp)
{
    const char *embed_str =
        embed == PLATFORM_MULTI  ? "multi-arch" :
        embed == PLATFORM_SINGLE ? "single-arch" : "none";

    const char *ready_str =
        (embed == PLATFORM_NONE)  ? "no-platform" :
        (build_cc < 0)            ? "no-compiler"  : "ready";

    ShJsonWriter w;
    sh_json_writer_init(&w, stdio_write_fn, f);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "version",            HL_VERSION);
    sh_json_write_kv_string(&w, "runtime",            doctor_runtime());
    sh_json_write_kv_string(&w, "platform",           doctor_platform());
    sh_json_write_kv_string(&w, "build",              doctor_build());
    sh_json_write_kv_string(&w, "platform_embedded",  embed_str);
    /* Additive, from the Windows-onboarding pass: the HOST this binary runs on
     * (distinct from "platform", which is just "cosmo" for every cosmo build),
     * the artifact suffix that host needs, which compiler actually satisfies
     * `hull build` here, and the one command that fixes it when none does.
     * Agents and installers read these instead of scraping the human text. */
    sh_json_write_kv_string(&w, "host_os",            hl_host_os());
    sh_json_write_kv_string(&w, "exe_suffix",         hl_host_exe_suffix());
    emit_string_or_null(&w, "build_compiler",
                        build_cc >= 0 ? ci[build_cc].name : NULL);
    sh_json_write_kv_string(&w, "build_compiler_required",
                            doctor_wants_cosmocc() ? "cosmocc"
                                                   : "cc|gcc|clang");
    emit_string_or_null(&w, "fix_command",
                        (build_cc < 0) ? doctor_compiler_fix() : NULL);

    sh_json_write_key(&w, "compilers");
    sh_json_write_array_start(&w);
    for (int i = 0; i < nci; i++) {
        sh_json_write_object_start(&w);
        sh_json_write_kv_string(&w, "name", ci[i].name);
        emit_string_or_null(&w, "path", ci[i].path);
        sh_json_write_object_end(&w);
    }
    sh_json_write_array_end(&w);

    /* CA bundle status */
    {
        const unsigned char *cab_data = NULL;
        size_t cab_len = 0;
        int cab_embedded = (hl_embedded_ca_bundle(&cab_data, &cab_len) == 0);
        const char *system_found = doctor_system_ca();
        sh_json_write_key(&w, "ca_bundle");
        sh_json_write_object_start(&w);
        emit_string_or_null(&w, "system", system_found);
        sh_json_write_kv_bool(&w, "embedded", cab_embedded != 0);
        if (cab_embedded)
            sh_json_write_kv_string(&w, "embedded_label",
                                    hl_embedded_ca_bundle_label());
        /* Which store HTTPS actually uses, and whether HTTPS works at all. An
         * absent system store with the embedded bundle present is a HEALTHY
         * state; "ok" says so without a consumer having to infer it. */
        sh_json_write_kv_string(&w, "effective",
                                system_found ? "system"
                                             : (cab_embedded ? "embedded"
                                                             : "none"));
        sh_json_write_kv_bool(&w, "ok",
                              (system_found != NULL) || cab_embedded);
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

    /* Module-subsystem capability bits - mirrors build_provided_caps()
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

    /* Cache status - same registry as `hull cache list`. */
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
    discover_compilers(ci, MAX_COMPILERS, NULL);

    int build_cc = doctor_build_compiler(ci, MAX_COMPILERS);

    PlatformEmbed embed = detect_platform();

    ComputeInfo cmp;
    /* hull_exe lookup not available here - pass NULL; the sibling-to-hull
     * probe is skipped, but ~/.hull/tools + PATH resolution still work. */
    detect_compute(&cmp, NULL);

    print_json(f, ci, MAX_COMPILERS, embed, build_cc, &cmp);
}

/* ── Handler ─────────────────────────────────────────────────────── */

int hl_cmd_doctor(int argc, char **argv, const HlCommandEnv *env)
{
    int json = 0;
    int tui  = 0;
    int fix  = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0)
            json = 1;
        else if (strcmp(argv[i], "--tui") == 0)
            tui = 1;
        else if (strcmp(argv[i], "--fix") == 0)
            fix = 1;
    }

    if (fix && json) {
        fprintf(stderr, "hull doctor: --fix and --json are mutually exclusive "
                        "(--fix produces interactive output)\n");
        return 2;
    }

#ifdef HL_TUI_LINKED
    if (tui) {
        /* Refuse cleanly when not attached to a real terminal - the
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
    discover_compilers(ci, MAX_COMPILERS, env->hull_exe);

    int build_cc = doctor_build_compiler(ci, MAX_COMPILERS);

    PlatformEmbed embed = detect_platform();

    ComputeInfo cmp;
    detect_compute(&cmp, env->hull_exe);

    /* --fix installs the HULL-MANAGED prerequisite doctor just found missing,
     * and nothing else. Deliberate boundaries:
     *
     *   - It only ever runs a command doctor would have PRINTED, through the
     *     ordinary `hull tools install` path - same signed-manifest trust
     *     chain (Ed25519 over hull.sha256), same ~/.hull/tools destination.
     *     No new download route, no new trust anchor.
     *   - It never touches PATH, the registry, or any system directory.
     *     Nothing outside ~/.hull is modified, so there is no surprise
     *     system-wide change hiding behind the flag.
     *   - It never installs OPTIONAL capabilities (wamrc, gpu). Those are
     *     choices, not repairs.
     *   - It states exactly what it is about to do before doing it.
     *
     * When no Hull-managed fix exists (a native hull with no system
     * toolchain) it says so plainly instead of pretending to act. */
    if (fix) {
        if (embed == PLATFORM_NONE) {
            fprintf(stderr,
                "hull doctor --fix: this hull has no embedded platform "
                "library, which --fix cannot supply.\n"
                "  Install an official release, or build from source with "
                "`make EMBED_PLATFORM=1`.\n");
            return 1;
        }
        if (build_cc >= 0) {
            printf("hull doctor --fix: nothing to do - `hull build` is "
                   "already ready (%s).\n", ci[build_cc].name);
            return 0;
        }
        const char *fixcmd = doctor_compiler_fix();
        if (!fixcmd) {
            fprintf(stderr,
                "hull doctor --fix: no Hull-managed fix exists on this "
                "platform.\n"
                "  Install a C toolchain (%s) with your package manager, "
                "then re-run `hull doctor`.\n",
                doctor_wants_cosmocc() ? "cosmocc" : "gcc or clang");
            return 1;
        }
#ifdef HL_ENABLE_HTTP_CLIENT
        printf("hull doctor --fix: `hull build` needs a C toolchain and none "
               "is installed.\n");
        printf("  Running: %s\n", fixcmd);
        printf("  This downloads a signed Hull toolchain bundle into "
               "~/.hull/tools/ .\n");
        printf("  Nothing outside ~/.hull is modified.\n\n");
        {
            char *targv[] = { (char *)"tools", (char *)"install",
                              (char *)"cosmocc" };
            int rc = hl_cmd_tools(3, targv, env);
            if (rc != 0) {
                fprintf(stderr, "\nhull doctor --fix: `%s` failed (exit %d). "
                                "Run it directly for the full output.\n",
                        fixcmd, rc);
                return rc;
            }
        }
        printf("\nhull doctor --fix: done. Re-run `hull doctor` to confirm.\n");
        return 0;
#else
        fprintf(stderr,
            "hull doctor --fix: this build has no HTTP client "
            "(HL_ENABLE_HTTP_CLIENT=0), so it cannot download `%s`.\n",
            fixcmd);
        return 1;
#endif
    }

    if (json)
        print_json(stdout, ci, MAX_COMPILERS, embed, build_cc, &cmp);
    else
        print_human(stdout, ci, MAX_COMPILERS, embed, build_cc, &cmp);

    /* Exit 1 if hull build cannot work, so scripts can check: hull doctor || ...
     * "A compiler exists" is not enough: it must be the compiler THIS binary
     * can actually link with (cosmocc on a cosmo hull), or doctor would
     * greenlight a build that fails at the link step. */
    int build_ready = (embed != PLATFORM_NONE) && (build_cc >= 0);
    return build_ready ? 0 : 1;
}
