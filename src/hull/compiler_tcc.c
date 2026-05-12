#ifdef HL_ENABLE_TCC
/*
 * compiler_tcc.c — Embedded TinyCC compile-only backend
 *
 * Compile step: tcc -c -nostdinc [-I include_dir] -o obj src
 * Link step: delegates to first available system linker (cc/gcc/clang/ld)
 *
 * The tcc binary is extracted from the embedded byte array on first use.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/compiler.h"
#include "hull/build_assets.h"
#include "hull/cap/tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#define HL_COMPILER_MAX_ARGS 256

typedef struct {
    char tcc_path[PATH_MAX];
    char tmp_dir[PATH_MAX];
    int  extracted;
} TccCtx;

static int tcc_ensure_extracted(TccCtx *ctx)
{
    if (ctx->extracted) return 0;

    char tmpl[] = "/tmp/hull_tcc_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) return -1;
    int dn = snprintf(ctx->tmp_dir, sizeof(ctx->tmp_dir), "%s", dir);
    if (dn < 0 || (size_t)dn >= sizeof(ctx->tmp_dir))
        return -1;

    if (hl_build_extract_tcc(ctx->tmp_dir) != 0)
        return -1;

    int tn = snprintf(ctx->tcc_path, sizeof(ctx->tcc_path),
                      "%s/tcc", ctx->tmp_dir);
    if (tn < 0 || (size_t)tn >= sizeof(ctx->tcc_path))
        return -1;
    ctx->extracted = 1;
    return 0;
}

static const char *tcc_name(HlCompiler *c) { (void)c; return "tcc"; }

static int tcc_is_available(HlCompiler *c)
{
    TccCtx *ctx = (TccCtx *)c->ctx;
    /* tcc produces ELF objects; not useful on cosmo (APE archives) or macOS (Mach-O) */
    if (hl_build_get_platforms(NULL) > 0) return 0;
#if defined(__APPLE__)
    (void)ctx;
    return 0;
#else
    return tcc_ensure_extracted(ctx) == 0;
#endif
}

static char *tcc_version(HlCompiler *c)
{
    (void)c;
    return strdup(hl_build_tcc_version_string());
}

static int tcc_compile(HlCompiler *c, const char *src, const char *obj,
                       const char *include_dir)
{
    TccCtx *ctx = (TccCtx *)c->ctx;
    if (tcc_ensure_extracted(ctx) != 0) return -1;

    const char *argv[HL_COMPILER_MAX_ARGS];
    int n = 0;
    argv[n++] = ctx->tcc_path;
    argv[n++] = "-c";
    argv[n++] = "-nostdinc";
    if (include_dir) { argv[n++] = "-I"; argv[n++] = include_dir; }
    argv[n++] = "-o"; argv[n++] = obj;
    argv[n++] = src;
    argv[n]   = NULL;
    return hl_tool_spawn(argv) == 0 ? 0 : -1;
}

static int tcc_link(HlCompiler *c, const char *output,
                    const char **objs, const char **libs)
{
    (void)c;
    /* Delegate to the first available system linker */
    static const char *linkers[] = { "cc", "gcc", "clang", NULL };
    for (const char **p = linkers; *p; p++) {
        HlCompiler *sys = hl_compiler_system_new(*p);
        if (!sys) continue;
        if (!hl_compiler_is_available(sys)) { hl_compiler_destroy(sys); continue; }
        int rc = hl_compiler_link(sys, output, objs, libs);
        hl_compiler_destroy(sys);
        return rc;
    }
    fprintf(stderr, "hull build: tcc: no system linker found in PATH "
                    "(install gcc or cc for the link step)\n");
    return -1;
}

static void tcc_destroy(HlCompiler *c)
{
    TccCtx *ctx = (TccCtx *)c->ctx;
    /* Optionally clean up extracted tcc — leave for OS to clean /tmp */
    (void)ctx;
    free(c->ctx);
}

static const HlCompilerVtable tcc_vtable = {
    tcc_name, tcc_is_available, tcc_version,
    tcc_compile, tcc_link, tcc_destroy
};

HlCompiler *hl_compiler_tcc_new(void)
{
    TccCtx *ctx = (TccCtx *)calloc(1, sizeof(TccCtx));
    if (!ctx) return NULL;
    HlCompiler *c = (HlCompiler *)malloc(sizeof(HlCompiler));
    if (!c) { free(ctx); return NULL; }
    c->vtable = &tcc_vtable;
    c->ctx    = ctx;
    return c;
}

#endif /* HL_ENABLE_TCC */
