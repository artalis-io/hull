/*
 * compiler.c — System compiler backend + compiler selection
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/compiler.h"
#include "hull/cap/tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Maximum argv entries for compile/link commands */
#define HL_COMPILER_MAX_ARGS 256

/* ── System backend ─────────────────────────────────────────────── */

typedef struct {
    char cc[PATH_MAX];
} SysCtx;

static const char *sys_name(HlCompiler *c)
{
    SysCtx *ctx = (SysCtx *)c->ctx;
    /* Return just the basename */
    const char *slash = strrchr(ctx->cc, '/');
    return slash ? slash + 1 : ctx->cc;
}

static int sys_is_available(HlCompiler *c)
{
    SysCtx *ctx = (SysCtx *)c->ctx;
    const char *argv[] = { ctx->cc, "--version", NULL };
    char *out = hl_tool_spawn_read(argv, NULL);
    if (out) { free(out); return 1; }
    return 0;
}

static char *sys_version(HlCompiler *c)
{
    SysCtx *ctx = (SysCtx *)c->ctx;
    const char *argv[] = { ctx->cc, "--version", NULL };
    char *out = hl_tool_spawn_read(argv, NULL);
    if (!out) return NULL;
    char *nl = strchr(out, '\n');
    if (nl) *nl = '\0';
    return out; /* caller frees */
}

static int sys_compile(HlCompiler *c, const char *src, const char *obj,
                       const char *include_dir)
{
    SysCtx *ctx = (SysCtx *)c->ctx;
    const char *argv[HL_COMPILER_MAX_ARGS];
    int n = 0;
    argv[n++] = ctx->cc;
    argv[n++] = "-std=c11"; argv[n++] = "-O2"; argv[n++] = "-w";
    argv[n++] = "-c";
    if (include_dir) { argv[n++] = "-I"; argv[n++] = include_dir; }
    argv[n++] = "-o"; argv[n++] = obj;
    argv[n++] = src;
    argv[n]   = NULL;
    return hl_tool_spawn(argv) == 0 ? 0 : -1;
}

static int sys_link(HlCompiler *c, const char *output,
                    const char **objs, const char **libs)
{
    SysCtx *ctx = (SysCtx *)c->ctx;
    const char *argv[HL_COMPILER_MAX_ARGS];
    int n = 0;
    argv[n++] = ctx->cc;
    argv[n++] = "-o"; argv[n++] = output;
    /* Reproducibility note: GNU ld's default `--build-id=sha1` already
     * produces content-addressed Build-IDs (deterministic for identical
     * inputs) so apps `hull build` produces on Linux are reproducible
     * without an explicit flag. An earlier attempt to inject
     * `-Wl,--build-id=none` here broke test_compiler's link tests via
     * the TCC backend's sys_link delegation; the default behavior is
     * already correct, so no flag is needed. macOS LC_UUID is a
     * separate (unfixable-here) story tracked in roadmap_next.md §0.2. */
    for (int i = 0; objs && objs[i] && n < HL_COMPILER_MAX_ARGS - 2; i++)
        argv[n++] = objs[i];
    for (int i = 0; libs && libs[i] && n < HL_COMPILER_MAX_ARGS - 2; i++)
        argv[n++] = libs[i];
    argv[n] = NULL;
    return hl_tool_spawn(argv) == 0 ? 0 : -1;
}

static void sys_destroy(HlCompiler *c)
{
    free(c->ctx);
    /* caller frees c itself via hl_compiler_destroy macro */
}

static const HlCompilerVtable sys_vtable = {
    sys_name, sys_is_available, sys_version,
    sys_compile, sys_link, sys_destroy
};

HlCompiler *hl_compiler_system_new(const char *cc_path)
{
    if (!cc_path) return NULL;
    SysCtx *ctx = (SysCtx *)malloc(sizeof(SysCtx));
    if (!ctx) return NULL;
    snprintf(ctx->cc, sizeof(ctx->cc), "%s", cc_path);
    HlCompiler *c = (HlCompiler *)malloc(sizeof(HlCompiler));
    if (!c) { free(ctx); return NULL; }
    c->vtable = &sys_vtable;
    c->ctx    = ctx;
    return c;
}

/* ── Selection ──────────────────────────────────────────────────── */

HlCompiler *hl_compiler_select(const char *explicit_cc)
{
    /* Explicit "tcc" sentinel → tcc backend */
    if (explicit_cc && strcmp(explicit_cc, "tcc") == 0) {
#ifdef HL_ENABLE_TCC
        HlCompiler *t = hl_compiler_tcc_new();
        if (t && hl_compiler_is_available(t)) return t;
        if (t) hl_compiler_destroy(t);
        fprintf(stderr, "hull: tcc requested but not available on this "
                        "platform (cosmo APE archives or macOS Mach-O)\n");
#else
        fprintf(stderr, "hull: tcc not compiled into this build "
                        "(rebuild with HL_ENABLE_TCC=1)\n");
#endif
        return NULL;
    }

    /* Explicit "system" sentinel → skip tcc, force system auto-detect */
    int skip_tcc = (explicit_cc && strcmp(explicit_cc, "system") == 0);

    /* Explicit named compiler */
    if (explicit_cc && !skip_tcc) {
        HlCompiler *c = hl_compiler_system_new(explicit_cc);
        if (c && hl_compiler_is_available(c)) return c;
        if (c) hl_compiler_destroy(c);
        fprintf(stderr, "hull: compiler '%s' not found in PATH\n", explicit_cc);
        return NULL;
    }

#ifdef HL_ENABLE_TCC
    /* Auto: try embedded tcc first (if cosmo archives not present — checked
     * inside tcc_new via build_assets) */
    if (!skip_tcc) {
        HlCompiler *t = hl_compiler_tcc_new();
        if (t && hl_compiler_is_available(t)) return t;
        if (t) hl_compiler_destroy(t);
    }
#endif

#ifdef __COSMOPOLITAN__
    /* Cosmo hull: prefer cosmocc since the embedded platform .a's are
     * cosmo-format archives. Native cc/gcc/clang link them to ELF/Mach-O
     * (not APE), so the output wouldn't be a portable cosmo binary
     * anyway. Check known install locations before $PATH because the
     * cosmocc.zip installer typically doesn't put cosmocc on PATH by
     * default. §3.1 of roadmap_next.md. */
    {
        const char *home = getenv("HOME");
        char path[512];

        /* User-install paths first (cosmocc.zip default layouts) */
        if (home && *home) {
            int n = snprintf(path, sizeof(path),
                             "%s/.cosmocc/bin/cosmocc", home);
            if (n > 0 && (size_t)n < sizeof(path)) {
                HlCompiler *c = hl_compiler_system_new(path);
                if (c && hl_compiler_is_available(c)) return c;
                if (c) hl_compiler_destroy(c);
            }
            n = snprintf(path, sizeof(path),
                         "%s/cosmocc/bin/cosmocc", home);
            if (n > 0 && (size_t)n < sizeof(path)) {
                HlCompiler *c = hl_compiler_system_new(path);
                if (c && hl_compiler_is_available(c)) return c;
                if (c) hl_compiler_destroy(c);
            }
        }
        /* System-wide install (`make fetch-cosmocc COSMOCC_DIR=/opt/cosmo`,
         * package managers) */
        HlCompiler *c = hl_compiler_system_new("/opt/cosmo/bin/cosmocc");
        if (c && hl_compiler_is_available(c)) return c;
        if (c) hl_compiler_destroy(c);
        /* $PATH fallback */
        c = hl_compiler_system_new("cosmocc");
        if (c && hl_compiler_is_available(c)) return c;
        if (c) hl_compiler_destroy(c);
        /* Fall through to cc/gcc/clang below. Those won't actually
         * produce a working APE against cosmo .a's, but letting them
         * try gives a clearer link-time error than a silent
         * "no compiler found." */
    }
#endif

    /* Auto: try system compilers in PATH */
    static const char *candidates[] = { "cc", "gcc", "clang", NULL };
    for (const char **p = candidates; *p; p++) {
        HlCompiler *c = hl_compiler_system_new(*p);
        if (c && hl_compiler_is_available(c)) return c;
        if (c) hl_compiler_destroy(c);
    }

    return NULL;
}
